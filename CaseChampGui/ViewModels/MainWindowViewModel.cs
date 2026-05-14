using System;
using System.Collections.ObjectModel;
using System.IO;
using System.Linq;
using System.Threading.Tasks;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Media.Imaging;
using Avalonia.Styling;
using Avalonia.Threading;
using CaseChampGui.Models;
using CaseChampGui.Services;

namespace CaseChampGui.ViewModels;

public sealed class MainWindowViewModel : ObservableObject, IDisposable
{
    private readonly IDatabaseClient _client;
    private readonly ISettingsService _settingsService;
    private readonly ThemeService _themeService;
    private readonly SchemaService _schemaService;
    private readonly LocalServerService _localServer;

    private NavItemViewModel? _selectedNavItem;
    private AppSection _currentSection = AppSection.Sql;
    private string _connectionBadge = "Не подключено";
    private string _connectionBadgeIcon = "○";
    private bool _isConnected;
    private DatabaseTabViewModel? _selectedDatabase;
    private bool _isLoadingDatabases;

    private const double SidebarCollapsedWidth = 84;
    private const double SidebarExpandedWidth = 224;
    private bool _autoCollapseSidebar = true;
    private bool _isSidebarHovered;

    private bool _compactMode;
    private bool _animationsEnabled = true;
    private double _editorFontSize = 14;

    private bool _authOverlayVisible = true;
    private Bitmap? _accountAvatarBitmap;
    private Bitmap? _sidebarBrandBitmap;
    private string _accountPanelLabel = string.Empty;

    public MainWindowViewModel(
        IDatabaseClient client,
        ISettingsService settingsService,
        ThemeService themeService,
        SchemaService schemaService,
        LocalServerService localServer,
        NotificationService notifications,
        SqlViewModel sqlViewModel,
        Text2SqlViewModel text2SqlViewModel,
        SettingsViewModel settingsViewModel,
        Func<Task> authGateCompletion)
    {
        _client = client;
        _settingsService = settingsService;
        _themeService = themeService;
        _schemaService = schemaService;
        _localServer = localServer;
        Notifications = notifications;
        Sql = sqlViewModel;
        Text2Sql = text2SqlViewModel;
        Settings = settingsViewModel;

        Auth = new AuthViewModel(client, settingsService, authGateCompletion);

        NavItems = new ObservableCollection<NavItemViewModel>
        {
            new(AppSection.Sql, "SQL", "▣"),
            new(AppSection.Text2Sql, "Text2SQL", "✦"),
            new(AppSection.Settings, "Настройки", "⚙", isBottom: true),
        };

        PrimaryNavItems = new ObservableCollection<NavItemViewModel>();
        BottomNavItems = new ObservableCollection<NavItemViewModel>();
        foreach (var item in NavItems)
        {
            if (item.IsBottom) BottomNavItems.Add(item);
            else PrimaryNavItems.Add(item);
        }
        SelectedNavItem = NavItems[0];

        RefreshDatabasesCommand = new AsyncRelayCommand(LoadDatabasesAsync, () => !_isLoadingDatabases);
        LogoutCommand = new AsyncRelayCommand(LogoutAsyncImpl, () => !AuthOverlayVisible);

        _client.StateChanged += OnClientStateChanged;
        _settingsService.SettingsChanged += OnSettingsChanged;
        _themeService.ThemeApplied += (_, _) => RefreshSidebarBrand();
        Sql.SchemaInvalidationRequested += OnSchemaInvalidationRequested;
        Settings.ChatModeChanged += (_, enabled) => Sql.IsChatMode = enabled;
        Settings.SidebarAutoCollapseChanged += (_, value) => AutoCollapseSidebar = value;
        Settings.CompactModeChanged += (_, value) => CompactMode = value;
        Settings.AnimationsEnabledChanged += (_, value) => AnimationsEnabled = value;
        Settings.EditorFontSizeChanged += (_, value) => EditorFontSize = value;

        _compactMode = settingsViewModel.CompactMode;
        _animationsEnabled = settingsViewModel.AnimationsEnabled;
        _editorFontSize = settingsViewModel.EditorFontSize;
        _autoCollapseSidebar = settingsViewModel.SidebarAutoCollapse;

        UpdateConnectionBadge();
        RefreshAccountAppearance();
        RefreshSidebarBrand();
    }

    public Bitmap? SidebarBrandBitmap
    {
        get => _sidebarBrandBitmap;
        private set
        {
            var old = _sidebarBrandBitmap;
            if (!SetProperty(ref _sidebarBrandBitmap, value)) return;
            old?.Dispose();
        }
    }

    public AuthViewModel Auth { get; }

    public bool AuthOverlayVisible
    {
        get => _authOverlayVisible;
        private set
        {
            if (SetProperty(ref _authOverlayVisible, value))
            {
                LogoutCommand.RaiseCanExecuteChanged();
            }
        }
    }

    public Task CompleteAuthenticationGateAsync()
    {
        var d = Dispatcher.UIThread;
        if (d.CheckAccess())
            return CompleteAuthenticationGateCoreAsync();
        return d.InvokeAsync(CompleteAuthenticationGateCoreAsync);
    }

    private async Task CompleteAuthenticationGateCoreAsync()
    {
        AuthOverlayVisible = false;
        await PostAuthBootstrapAsync();
        await LoadDatabasesAsync();
        RefreshAccountAppearance();
    }

    public string AccountPanelLabel
    {
        get => _accountPanelLabel;
        private set
        {
            if (SetProperty(ref _accountPanelLabel, value))
            {
                OnPropertyChanged(nameof(AccountInitial));
            }
        }
    }

    public string AccountInitial =>
        string.IsNullOrEmpty(AccountPanelLabel) || AccountPanelLabel == "…"
            ? "?"
            : char.ToUpperInvariant(AccountPanelLabel[0]).ToString();

    public bool IsAdminUser =>
        string.Equals(_client.CurrentUser, "admin", StringComparison.OrdinalIgnoreCase);

    public bool ShowAvatarOnPanel =>
        !IsAdminUser && AccountAvatarBitmap is not null;

    public Bitmap? AccountAvatarBitmap
    {
        get => _accountAvatarBitmap;
        private set
        {
            if (SetProperty(ref _accountAvatarBitmap, value))
            {
                OnPropertyChanged(nameof(ShowAvatarOnPanel));
            }
        }
    }

    public NotificationService Notifications { get; }
    public SqlViewModel Sql { get; }
    public Text2SqlViewModel Text2Sql { get; }
    public SettingsViewModel Settings { get; }

    public ObservableCollection<NavItemViewModel> NavItems { get; }
    public ObservableCollection<NavItemViewModel> PrimaryNavItems { get; }
    public ObservableCollection<NavItemViewModel> BottomNavItems { get; }
    public ObservableCollection<DatabaseTabViewModel> Databases { get; } = new();

    public AsyncRelayCommand LogoutCommand { get; }

    public bool HasDatabases => Databases.Count > 0;

    public NavItemViewModel? SelectedNavItem
    {
        get => _selectedNavItem;
        set
        {
            if (value is null) return;
            if (SetProperty(ref _selectedNavItem, value))
            {
                foreach (var item in NavItems)
                {
                    item.IsSelected = ReferenceEquals(item, value);
                }
                CurrentSection = value.Section;
            }
        }
    }

    public AppSection CurrentSection
    {
        get => _currentSection;
        private set
        {
            if (SetProperty(ref _currentSection, value))
            {
                OnPropertyChanged(nameof(IsSqlVisible));
                OnPropertyChanged(nameof(IsText2SqlVisible));
                OnPropertyChanged(nameof(IsSettingsVisible));
                OnPropertyChanged(nameof(CurrentSectionTitle));
                OnPropertyChanged(nameof(CurrentSectionSubtitle));
                OnPropertyChanged(nameof(CurrentSectionIcon));
                OnPropertyChanged(nameof(ShowDatabaseBar));
            }
        }
    }

    public bool IsSqlVisible => CurrentSection == AppSection.Sql;
    public bool IsText2SqlVisible => CurrentSection == AppSection.Text2Sql;
    public bool IsSettingsVisible => CurrentSection == AppSection.Settings;

    public bool ShowDatabaseBar => CurrentSection == AppSection.Sql;

    public string CurrentSectionTitle => CurrentSection switch
    {
        AppSection.Sql => "SQL Console",
        AppSection.Text2Sql => "Text2SQL",
        AppSection.Settings => "Настройки",
        _ => string.Empty,
    };

    public string CurrentSectionSubtitle => CurrentSection switch
    {
        AppSection.Sql => "Прямой ввод SQL и выполнение на сервере CaseChamp",
        AppSection.Text2Sql => "Перевод запросов с русского языка в SQL (скоро)",
        AppSection.Settings => "Параметры приложения и подключения",
        _ => string.Empty,
    };

    public string CurrentSectionIcon => CurrentSection switch
    {
        AppSection.Sql => "▣",
        AppSection.Text2Sql => "✦",
        AppSection.Settings => "⚙",
        _ => string.Empty,
    };

    public string ConnectionBadge
    {
        get => _connectionBadge;
        private set => SetProperty(ref _connectionBadge, value);
    }

    public string ConnectionBadgeIcon
    {
        get => _connectionBadgeIcon;
        private set => SetProperty(ref _connectionBadgeIcon, value);
    }

    public bool IsConnected
    {
        get => _isConnected;
        private set
        {
            if (SetProperty(ref _isConnected, value))
            {
                OnPropertyChanged(nameof(IsDisconnected));
            }
        }
    }

    public bool IsDisconnected => !_isConnected;

    public DatabaseTabViewModel? SelectedDatabase
    {
        get => _selectedDatabase;
        set
        {
            if (value is null || ReferenceEquals(value, _selectedDatabase)) return;
            SetProperty(ref _selectedDatabase, value);
            foreach (var db in Databases)
            {
                db.IsActive = ReferenceEquals(db, value);
            }
            _ = UseDatabaseAsync(value.Name);
        }
    }

    public AsyncRelayCommand RefreshDatabasesCommand { get; }

    public bool AutoCollapseSidebar
    {
        get => _autoCollapseSidebar;
        set
        {
            if (SetProperty(ref _autoCollapseSidebar, value))
            {
                OnPropertyChanged(nameof(IsSidebarExpanded));
                OnPropertyChanged(nameof(SidebarWidth));
            }
        }
    }

    public bool IsSidebarHovered
    {
        get => _isSidebarHovered;
        set
        {
            if (SetProperty(ref _isSidebarHovered, value))
            {
                OnPropertyChanged(nameof(IsSidebarExpanded));
                OnPropertyChanged(nameof(SidebarWidth));
            }
        }
    }

    public bool IsSidebarExpanded => !AutoCollapseSidebar || IsSidebarHovered;

    public double SidebarWidth => IsSidebarExpanded ? SidebarExpandedWidth : SidebarCollapsedWidth;

    public bool CompactMode
    {
        get => _compactMode;
        set
        {
            if (SetProperty(ref _compactMode, value))
            {
                OnPropertyChanged(nameof(ContentMargin));
                OnPropertyChanged(nameof(NavItemHeight));
                OnPropertyChanged(nameof(SectionHeaderPadding));
            }
        }
    }

    public bool AnimationsEnabled
    {
        get => _animationsEnabled;
        set => SetProperty(ref _animationsEnabled, value);
    }

    public double EditorFontSize
    {
        get => _editorFontSize;
        set => SetProperty(ref _editorFontSize, value);
    }

    public Thickness ContentMargin => _compactMode
        ? new Thickness(18, 12, 18, 16)
        : new Thickness(28, 20, 28, 24);

    public Thickness SectionHeaderPadding => _compactMode
        ? new Thickness(22, 12, 22, 10)
        : new Thickness(32, 18, 28, 16);

    public double NavItemHeight => _compactMode ? 32 : 40;

    public async Task InitializeAsync()
    {
        await _settingsService.LoadAsync();
        var s = _settingsService.Current;
        _themeService.Apply(s.Theme);
        Sql.IsChatMode = s.ChatModeEnabled;
        AutoCollapseSidebar = s.SidebarAutoCollapse;
        _client.Configure(s.Host, s.Port);
        RefreshAccountAppearance();
        RefreshSidebarBrand();

        if (!s.AutoConnect)
        {
            AuthOverlayVisible = false;
            return;
        }

        var ok = await _client.PingAsync();
        if (!ok && s.AutoStartLocalServer)
        {
            ok = await TryStartLocalServerAsync(s.Host, s.Port);
        }

        if (!ok)
        {
            AuthOverlayVisible = true;
            Auth.StatusText = "Нет связи с сервером. Проверьте настройки или запустите dbserver.";
            return;
        }

        if (await Auth.TrySilentLoginAsync())
        {
            return;
        }

        AuthOverlayVisible = true;
    }

    private async Task PostAuthBootstrapAsync()
    {
        try
        {
            var user = _client.CurrentUser ?? string.Empty;
            if (string.Equals(user, "admin", StringComparison.OrdinalIgnoreCase))
            {
                var use = await _client.ExecuteAsync("USE system;").ConfigureAwait(false);
                if (!use.Success)
                {
                    Notifications.Push("База system", use.Message, NotificationKind.Warning);
                }
            }
            else
            {
                var names = await _schemaService.GetDatabasesAsync().ConfigureAwait(false);
                var first = names.FirstOrDefault(n =>
                    !string.Equals(n, "system", StringComparison.OrdinalIgnoreCase));
                if (!string.IsNullOrEmpty(first))
                {
                    await _schemaService.UseDatabaseAsync(first).ConfigureAwait(false);
                    Sql.Schema.CurrentDatabase = first;
                    await Sql.Schema.RefreshAsync().ConfigureAwait(false);
                }
            }
        }
        catch (Exception ex)
        {
            Notifications.Push("Контекст после входа", ex.Message, NotificationKind.Warning);
        }
    }

    private Task LogoutAsyncImpl()
    {
        return Dispatcher.UIThread.InvokeAsync(async () =>
        {
            await _client.LogoutAsync();
            Databases.Clear();
            OnPropertyChanged(nameof(HasDatabases));
            _selectedDatabase = null;
            OnPropertyChanged(nameof(SelectedDatabase));
            Sql.Schema.CurrentDatabase = string.Empty;
            await Sql.Schema.RefreshAsync();
            AuthOverlayVisible = true;
            RefreshAccountAppearance();
        });
    }

    private void OnSettingsChanged(object? sender, AppSettings e)
    {
        RefreshAccountAppearance();
        RefreshSidebarBrand();
    }

    public void RefreshSidebarBrand()
    {
        Bitmap? created = null;
        try
        {
            created = BrandMarkBitmapFactory.CreateSidebarBrand(ResolveSidebarBrandVariant());
        }
        catch
        {
            created = null;
        }

        void Assign() => SidebarBrandBitmap = created;

        if (Dispatcher.UIThread.CheckAccess())
            Assign();
        else
            Dispatcher.UIThread.Post(Assign);
    }

    private SidebarBrandVariant ResolveSidebarBrandVariant()
    {
        return _themeService.CurrentTheme switch
        {
            AppTheme.Light => SidebarBrandVariant.Light,
            AppTheme.Dark => SidebarBrandVariant.Dark,
            AppTheme.Iu5 => SidebarBrandVariant.Iu5,
            AppTheme.System => global::Avalonia.Application.Current?.ActualThemeVariant == ThemeVariant.Light
                ? SidebarBrandVariant.Light
                : SidebarBrandVariant.Dark,
            _ => SidebarBrandVariant.Dark,
        };
    }

    private void RefreshAccountAppearance()
    {
        Dispatcher.UIThread.Post(() =>
        {
            var s = _settingsService.Current;
            var user = _client.CurrentUser ?? string.Empty;
            var label = string.IsNullOrWhiteSpace(s.DisplayNameOverride) ? user : s.DisplayNameOverride!;
            AccountPanelLabel = string.IsNullOrEmpty(label) ? "…" : label;

            var old = AccountAvatarBitmap;
            AccountAvatarBitmap = null;
            old?.Dispose();

            OnPropertyChanged(nameof(IsAdminUser));
            OnPropertyChanged(nameof(ShowAvatarOnPanel));

            if (!IsAdminUser && !string.IsNullOrEmpty(s.AvatarFilePath) && File.Exists(s.AvatarFilePath))
            {
                try
                {
                    AccountAvatarBitmap = new Bitmap(s.AvatarFilePath);
                }
                catch
                {
                    AccountAvatarBitmap = null;
                }
            }

            OnPropertyChanged(nameof(ShowAvatarOnPanel));
        });
    }

    private async Task<bool> TryStartLocalServerAsync(string host, int port)
    {
        var exe = _localServer.FindExecutable();
        if (exe is null)
        {
            Notifications.Push(
                "Локальный сервер не найден",
                "Не нашёл build/dbserver. Соберите проект (cmake --build build) или укажите хост вручную.",
                NotificationKind.Warning);
            return false;
        }

        Notifications.Push("Запуск локального dbserver...", $"Использую {exe}", NotificationKind.Info, TimeSpan.FromSeconds(4));
        try
        {
            var started = await _localServer.StartAsync(host, port, async ct => await _client.PingAsync(ct));
            if (started)
            {
                Notifications.Push("Локальный сервер готов", $"dbserver слушает {host}:{port}", NotificationKind.Info, TimeSpan.FromSeconds(4));
                return true;
            }
            Notifications.Push(
                "Не удалось запустить dbserver",
                "Процесс не ответил на /ping вовремя. Проверьте, что порт свободен.",
                NotificationKind.Error);
            return false;
        }
        catch (Exception ex)
        {
            Notifications.Push("Ошибка запуска dbserver", ex.Message, NotificationKind.Error);
            return false;
        }
    }

    private async Task LoadDatabasesAsync()
    {
        _isLoadingDatabases = true;
        RefreshDatabasesCommand.RaiseCanExecuteChanged();
        try
        {
            var names = await _schemaService.GetDatabasesAsync();
            Databases.Clear();
            foreach (var name in names)
            {
                Databases.Add(new DatabaseTabViewModel(name));
            }
            OnPropertyChanged(nameof(HasDatabases));

            var currentDb = _client.CurrentDb;
            if (!string.IsNullOrEmpty(currentDb))
            {
                foreach (var db in Databases)
                {
                    if (string.Equals(db.Name, currentDb, StringComparison.Ordinal))
                    {
                        db.IsActive = true;
                        _selectedDatabase = db;
                        OnPropertyChanged(nameof(SelectedDatabase));
                        Sql.Schema.CurrentDatabase = db.Name;
                        await Sql.Schema.RefreshAsync();
                        break;
                    }
                }
            }
        }
        catch (Exception ex)
        {
            Notifications.Push("Не удалось получить список БД", ex.Message, NotificationKind.Error);
        }
        finally
        {
            _isLoadingDatabases = false;
            RefreshDatabasesCommand.RaiseCanExecuteChanged();
        }
    }

    private async Task UseDatabaseAsync(string database)
    {
        try
        {
            var ok = await _schemaService.UseDatabaseAsync(database);
            if (!ok)
            {
                Notifications.Push("Ошибка", $"Не удалось выбрать базу '{database}'", NotificationKind.Error);
                return;
            }
            Sql.Schema.CurrentDatabase = database;
            await Sql.Schema.RefreshAsync();
        }
        catch (Exception ex)
        {
            Notifications.Push("Ошибка переключения БД", ex.Message, NotificationKind.Error);
        }
    }

    private async void OnSchemaInvalidationRequested(object? sender, string sql)
    {
        try
        {
            await LoadDatabasesAsync();
            if (!string.IsNullOrEmpty(_client.CurrentDb))
            {
                Sql.Schema.CurrentDatabase = _client.CurrentDb;
                await Sql.Schema.RefreshAsync();
            }
        }
        catch (Exception ex)
        {
            Notifications.Push("Ошибка обновления схемы", ex.Message, NotificationKind.Warning);
        }
    }

    private void OnClientStateChanged(object? sender, EventArgs e)
    {
        Dispatcher.UIThread.Post(async () =>
        {
            UpdateConnectionBadge();
            if (_isConnected && Databases.Count == 0 && !AuthOverlayVisible)
            {
                await LoadDatabasesAsync();
            }
        });
    }

    private void UpdateConnectionBadge()
    {
        IsConnected = _client.Status == ConnectionStatus.Connected;
        ConnectionBadge = _client.Status switch
        {
            ConnectionStatus.Connected => $"{_client.Host}:{_client.Port}",
            ConnectionStatus.Connecting => "Подключение...",
            ConnectionStatus.Failed => "Нет связи",
            _ => "Офлайн",
        };
        ConnectionBadgeIcon = _client.Status switch
        {
            ConnectionStatus.Connected => "●",
            ConnectionStatus.Connecting => "◐",
            ConnectionStatus.Failed => "✕",
            _ => "○",
        };
    }

    public void Dispose()
    {
        _client.StateChanged -= OnClientStateChanged;
        _settingsService.SettingsChanged -= OnSettingsChanged;
        Sql.SchemaInvalidationRequested -= OnSchemaInvalidationRequested;
        AccountAvatarBitmap?.Dispose();
        SidebarBrandBitmap = null;
    }
}
