using System;
using System.Collections.ObjectModel;
using System.IO;
using System.Linq;
using System.Threading;
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
    private const double NarrowWindowMaxWidth = 1060;

    private bool _autoCollapseSidebar = true;
    private bool _isSidebarHovered;
    private double _windowWidth = 1280;

    private bool _compactMode;
    private bool _animationsEnabled = true;
    private double _editorFontSize = 14;

    private bool _authOverlayVisible = true;
    private bool _authSettingsOverlayVisible;
    private bool _isFinishingAuth;
    private readonly CancellationTokenSource _startupCts = new();
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
        TableBrowseViewModel tableBrowseViewModel,
        SettingsViewModel settingsViewModel,
        SchemaPaneViewModel schemaPaneViewModel,
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
        Browse = tableBrowseViewModel;
        Settings = settingsViewModel;

        Auth = new AuthViewModel(client, settingsService, authGateCompletion, CancelStartupInitialization);

        NavItems = new ObservableCollection<NavItemViewModel>
        {
            new(AppSection.Sql, "SQL", "▣"),
            new(AppSection.Text2Sql, "Text2SQL", "✦"),
            new(AppSection.Browse, "Таблицы", "▤"),
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

        RefreshDatabasesCommand = new AsyncRelayCommand(
            () => LoadDatabasesAsync(refreshSchema: true),
            () => !_isLoadingDatabases);
        LogoutCommand = new AsyncRelayCommand(LogoutAsyncImpl, () => !AuthOverlayVisible);
        ToggleAuthSettingsCommand = new RelayCommand(() => AuthSettingsOverlayVisible = !AuthSettingsOverlayVisible);

        _client.StateChanged += OnClientStateChanged;
        _settingsService.SettingsChanged += OnSettingsChanged;
        _themeService.ThemeApplied += (_, _) => RefreshSidebarBrand();
        Sql.SchemaInvalidationRequested += OnSchemaInvalidationRequested;
        Text2Sql.CatalogSqlExecuted += OnSchemaInvalidationRequested;
        schemaPaneViewModel.OpenTableRequested += OnOpenTableFromSchema;
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
        UiThread.Post(RefreshCommandStates);
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
                OnPropertyChanged(nameof(IsMainChromeVisible));
                LogoutCommand.RaiseCanExecuteChanged();
                RefreshCommandStates();
            }
        }
    }

    public bool AuthSettingsOverlayVisible
    {
        get => _authSettingsOverlayVisible;
        private set
        {
            if (SetProperty(ref _authSettingsOverlayVisible, value))
            {
                Settings.IsPreAuthMode = value;
                ToggleAuthSettingsCommand.RaiseCanExecuteChanged();
            }
        }
    }

    public RelayCommand ToggleAuthSettingsCommand { get; }

    public bool IsMainChromeVisible => !AuthOverlayVisible;

    public Task CompleteAuthenticationGateAsync() =>
        UiThread.RunAsync(CompleteAuthenticationGateCoreAsync);

    private async Task CompleteAuthenticationGateCoreAsync()
    {
        _isFinishingAuth = true;
        Auth.SetStatus("Загрузка интерфейса…", isError: false);

        try
        {
            // Show main UI immediately so a slow /query does not leave a blank window.
            AuthOverlayVisible = false;
            AuthSettingsOverlayVisible = false;
            Auth.ClearStatus();
            RefreshAccountAppearance();
            OnPropertyChanged(nameof(IsAdminUser));

            await PostAuthBootstrapAsync().ConfigureAwait(false);
            await LoadDatabasesAsync(refreshSchema: false).ConfigureAwait(false);

            RefreshAccountAppearance();
            OnPropertyChanged(nameof(IsAdminUser));
        }
        catch (Exception ex)
        {
            AuthOverlayVisible = true;
            Auth.SetStatus($"Ошибка после входа: {ex.Message}", isError: true);
            Notifications.Push("Ошибка после входа", ex.Message, NotificationKind.Error);
        }
        finally
        {
            _isFinishingAuth = false;
            RefreshCommandStates();
        }
    }

    public void RefreshCommandStates()
    {
        void Refresh()
        {
            RefreshDatabasesCommand.RaiseCanExecuteChanged();
            LogoutCommand.RaiseCanExecuteChanged();
            Auth.SubmitCommand.RaiseCanExecuteChanged();
            Sql.RefreshCommandStates();
            Text2Sql.RefreshCommandStates();
            Settings.RefreshCommandStates();
        }

        if (Dispatcher.UIThread.CheckAccess())
            Refresh();
        else
            Dispatcher.UIThread.Post(Refresh);
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

    public bool IsAdminUser => _client.IsGlobalAdmin;

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
    public TableBrowseViewModel Browse { get; }
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
                OnPropertyChanged(nameof(IsBrowseVisible));
                OnPropertyChanged(nameof(IsSettingsVisible));
                OnPropertyChanged(nameof(CurrentSectionTitle));
                OnPropertyChanged(nameof(CurrentSectionSubtitle));
                OnPropertyChanged(nameof(CurrentSectionIcon));
                OnPropertyChanged(nameof(ShowDatabaseBar));
                if (value == AppSection.Text2Sql)
                    Text2Sql.OnSectionActivated();
                else if (value == AppSection.Browse)
                    Browse.OnSectionActivated();
            }
        }
    }

    public bool IsSqlVisible => CurrentSection == AppSection.Sql;
    public bool IsText2SqlVisible => CurrentSection == AppSection.Text2Sql;
    public bool IsBrowseVisible => CurrentSection == AppSection.Browse;
    public bool IsSettingsVisible => CurrentSection == AppSection.Settings;

    public bool ShowDatabaseBar =>
        CurrentSection is AppSection.Sql or AppSection.Text2Sql or AppSection.Browse;

    public string CurrentSectionTitle => CurrentSection switch
    {
        AppSection.Sql => "SQL Console",
        AppSection.Text2Sql => "Text2SQL",
        AppSection.Browse => "Таблицы",
        AppSection.Settings => "Настройки",
        _ => string.Empty,
    };

    public string CurrentSectionSubtitle => CurrentSection switch
    {
        AppSection.Sql => "Прямой ввод SQL и выполнение на сервере CaseChamp",
        AppSection.Text2Sql => "Перевод запросов с русского на SQL через Mistral AI",
        AppSection.Browse => "Просмотр данных выбранной таблицы (только чтение)",
        AppSection.Settings => "Параметры приложения и подключения",
        _ => string.Empty,
    };

    public string CurrentSectionIcon => CurrentSection switch
    {
        AppSection.Sql => "▣",
        AppSection.Text2Sql => "✦",
        AppSection.Browse => "▤",
        AppSection.Settings => "⚙",
        _ => string.Empty,
    };

    public void OpenBrowseTable(string tableName)
    {
        var browseNav = NavItems.FirstOrDefault(n => n.Section == AppSection.Browse);
        if (browseNav is not null)
            SelectedNavItem = browseNav;
        Browse.SelectAndLoadTable(tableName);
    }

    private void OnOpenTableFromSchema(object? sender, string tableName) => OpenBrowseTable(tableName);

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

    public double WindowWidth
    {
        get => _windowWidth;
        set
        {
            if (!SetProperty(ref _windowWidth, value)) return;
            if (IsNarrowWindow && _isSidebarHovered)
                IsSidebarHovered = false;
            OnPropertyChanged(nameof(IsSidebarExpanded));
            OnPropertyChanged(nameof(SidebarWidth));
        }
    }

    private bool IsNarrowWindow => _windowWidth < NarrowWindowMaxWidth;

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

    public bool IsSidebarExpanded =>
        !IsNarrowWindow && (!AutoCollapseSidebar || IsSidebarHovered);

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

    public void CancelStartupInitialization()
    {
        try
        {
            _startupCts.Cancel();
        }
        catch
        {
        }
    }

    public async Task InitializeAsync()
    {
        var ct = _startupCts.Token;
        try
        {
            await _settingsService.LoadAsync().ConfigureAwait(false);
            ct.ThrowIfCancellationRequested();

            var s = _settingsService.Current;
            await UiThread.RunAsync(() =>
            {
                _themeService.Apply(s.Theme);
                Sql.IsChatMode = s.ChatModeEnabled;
                AutoCollapseSidebar = s.SidebarAutoCollapse;
                _client.Configure(s.Host, s.Port);
                RefreshAccountAppearance();
                RefreshSidebarBrand();
            });

            if (!s.AutoConnect)
            {
                await UiThread.RunAsync(() =>
                {
                    if (!ShouldKeepAuthenticatedChrome())
                    {
                        AuthOverlayVisible = string.IsNullOrEmpty(_client.CurrentUser);
                    }
                });
                return;
            }

            var ok = await EnsureServerReachableAsync(ct).ConfigureAwait(false);
            ct.ThrowIfCancellationRequested();
            if (!ok)
            {
                await UiThread.RunAsync(() =>
                {
                    if (!ShouldKeepAuthenticatedChrome())
                    {
                        AuthOverlayVisible = true;
                        Auth.SetStatus("Нет связи с сервером. Проверьте настройки или запустите dbserver.", isError: true);
                    }
                });
                return;
            }

            if (await Auth.TrySilentLoginAsync(ct).ConfigureAwait(false))
            {
                return;
            }

            ct.ThrowIfCancellationRequested();
            await UiThread.RunAsync(() =>
            {
                if (!ShouldKeepAuthenticatedChrome())
                {
                    AuthOverlayVisible = true;
                }
            });
        }
        catch (OperationCanceledException)
        {
        }
        finally
        {
            await UiThread.RunAsync(RefreshCommandStates);
        }
    }

    private async Task PostAuthBootstrapAsync()
    {
        if (_client.IsGlobalAdmin)
        {
            return;
        }

        var names = await _schemaService.GetDatabasesAsync().ConfigureAwait(false);
        var first = names.FirstOrDefault(n =>
            !string.Equals(n, "system", StringComparison.OrdinalIgnoreCase));
        if (string.IsNullOrEmpty(first))
        {
            return;
        }

        await _schemaService.UseDatabaseAsync(first).ConfigureAwait(false);
        await PostToUiAsync(() => Sql.Schema.CurrentDatabase = first);
    }

    public Task<bool> EnsureServerReachableAsync(CancellationToken cancellationToken = default) =>
        EnsureServerReachableCoreAsync(cancellationToken);

    private async Task<bool> EnsureServerReachableCoreAsync(CancellationToken cancellationToken)
    {
        var s = _settingsService.Current;
        _client.Configure(s.Host, s.Port);
        if (await _client.PingAsync(cancellationToken).ConfigureAwait(false))
        {
            return true;
        }

        if (!s.AutoStartLocalServer)
        {
            return false;
        }

        return await TryStartLocalServerAsync(s.Host, s.Port, cancellationToken).ConfigureAwait(false);
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

    private async Task<bool> TryStartLocalServerAsync(string host, int port, CancellationToken cancellationToken = default)
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
            var started = await _localServer.StartAsync(
                host,
                port,
                async ct => await _client.PingAsync(ct).ConfigureAwait(false),
                cancellationToken).ConfigureAwait(false);
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

    private async Task LoadDatabasesAsync(bool refreshSchema = true)
    {
        await PostToUiAsync(() =>
        {
            _isLoadingDatabases = true;
            RefreshDatabasesCommand.RaiseCanExecuteChanged();
        });

        try
        {
            var names = await _schemaService.GetDatabasesAsync().ConfigureAwait(false);
            var currentDb = _client.CurrentDb;

            await PostToUiAsync(() =>
            {
                Databases.Clear();
                foreach (var name in names)
                {
                    Databases.Add(new DatabaseTabViewModel(name));
                }
                OnPropertyChanged(nameof(HasDatabases));

                if (string.IsNullOrEmpty(currentDb))
                {
                    return;
                }

                foreach (var db in Databases)
                {
                    if (!string.Equals(db.Name, currentDb, StringComparison.Ordinal))
                    {
                        continue;
                    }

                    db.IsActive = true;
                    _selectedDatabase = db;
                    OnPropertyChanged(nameof(SelectedDatabase));
                    Sql.Schema.CurrentDatabase = db.Name;
                    break;
                }
            });

            if (refreshSchema && !string.IsNullOrEmpty(currentDb))
            {
                await PostToUiAsync(() => Sql.Schema.RefreshAsync());
            }
        }
        catch (Exception ex)
        {
            Notifications.Push("Не удалось получить список БД", ex.Message, NotificationKind.Error);
        }
        finally
        {
            await PostToUiAsync(() =>
            {
                _isLoadingDatabases = false;
                RefreshDatabasesCommand.RaiseCanExecuteChanged();
            });
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
            await Browse.OnDatabaseChangedAsync();
        }
        catch (Exception ex)
        {
            Notifications.Push("Ошибка переключения БД", ex.Message, NotificationKind.Error);
        }
    }

    private void OnSchemaInvalidationRequested(object? sender, string sql)
    {
        _ = ReloadAfterDdlAsync(sql);
    }

    private async Task ReloadAfterDdlAsync(string sql)
    {
        try
        {
            var catalogOnly = IsDatabaseCatalogChange(sql);
            await LoadDatabasesAsync(refreshSchema: !catalogOnly).ConfigureAwait(false);

            if (catalogOnly || string.IsNullOrEmpty(_client.CurrentDb))
            {
                return;
            }

            await PostToUiAsync(async () =>
            {
                Sql.Schema.CurrentDatabase = _client.CurrentDb!;
                await Sql.Schema.RefreshAsync();
                await Browse.OnDatabaseChangedAsync();
            });
        }
        catch (Exception ex)
        {
            Notifications.Push("Ошибка обновления схемы", ex.Message, NotificationKind.Warning);
        }
    }

    private static bool IsDatabaseCatalogChange(string sql)
    {
        var t = sql.TrimStart();
        return t.StartsWith("CREATE DATABASE", StringComparison.OrdinalIgnoreCase)
               || t.StartsWith("DROP DATABASE", StringComparison.OrdinalIgnoreCase);
    }

    private bool ShouldKeepAuthenticatedChrome()
        => _isFinishingAuth || !string.IsNullOrEmpty(_client.CurrentUser);

    private void OnClientStateChanged(object? sender, EventArgs e)
    {
        UiThread.Post(() =>
        {
            UpdateConnectionBadge();
            if (_isConnected
                && Databases.Count == 0
                && !AuthOverlayVisible
                && !_isFinishingAuth
                && !string.IsNullOrEmpty(_client.CurrentUser))
            {
                _ = LoadDatabasesAsync(refreshSchema: false);
            }
        });
    }

    private static async Task PostToUiAsync(Action action)
    {
        if (Dispatcher.UIThread.CheckAccess())
        {
            action();
            return;
        }

        await Dispatcher.UIThread.InvokeAsync(action);
    }

    private static async Task PostToUiAsync(Func<Task> action)
    {
        if (Dispatcher.UIThread.CheckAccess())
        {
            await action();
            return;
        }

        await Dispatcher.UIThread.InvokeAsync(action);
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
        try
        {
            _startupCts.Cancel();
            _startupCts.Dispose();
        }
        catch
        {
        }

        _client.StateChanged -= OnClientStateChanged;
        _settingsService.SettingsChanged -= OnSettingsChanged;
        Sql.SchemaInvalidationRequested -= OnSchemaInvalidationRequested;
        Text2Sql.CatalogSqlExecuted -= OnSchemaInvalidationRequested;
        AccountAvatarBitmap?.Dispose();
        SidebarBrandBitmap = null;
    }
}
