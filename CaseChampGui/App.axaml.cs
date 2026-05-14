using System;
using System.Threading.Tasks;
using Avalonia;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Markup.Xaml;
using Avalonia.Media;
using Avalonia.Styling;
using Avalonia.Themes.Fluent;
using Avalonia.Threading;
using CaseChampGui.Services;
using CaseChampGui.ViewModels;
using CaseChampGui.Views;

namespace CaseChampGui;

public partial class App : Application
{
    private HttpDatabaseClient? _client;
    private NotificationService? _notifications;
    private LocalServerService? _localServer;

    public override void Initialize()
    {
        AvaloniaXamlLoader.Load(this);
    }

    public override void OnFrameworkInitializationCompleted()
    {
        ApplyFluentAccentPalettes();

        if (ApplicationLifetime is IClassicDesktopStyleApplicationLifetime desktop)
        {
            var settingsService = new JsonSettingsService();
            settingsService.Load();
            var themeService = new ThemeService();
            _client = new HttpDatabaseClient();
            var text2Sql = new DisabledText2SqlService();
            _notifications = new NotificationService();
            _localServer = new LocalServerService();
            var schemaService = new SchemaService(_client);
            var schemaPane = new SchemaPaneViewModel(schemaService);

            var sqlVm = new SqlViewModel(_client, schemaPane, _notifications);
            var text2SqlVm = new Text2SqlViewModel(text2Sql);
            var settingsVm = new SettingsViewModel(settingsService, themeService, _client);
            MainWindowViewModel? mainVm = null;
            mainVm = new MainWindowViewModel(
                _client, settingsService, themeService, schemaService, _localServer, _notifications,
                sqlVm, text2SqlVm, settingsVm,
                () => mainVm!.CompleteAuthenticationGateAsync());

            HookGlobalExceptionHandlers(_notifications);

            var window = new MainWindow { DataContext = mainVm };
            desktop.MainWindow = window;
            desktop.Exit += (_, _) =>
            {
                mainVm?.Dispose();
                _localServer?.Dispose();
                _client?.Dispose();
            };

            _ = SafeInitialize(mainVm);
        }

        base.OnFrameworkInitializationCompleted();
    }

    private void ApplyFluentAccentPalettes()
    {
        foreach (var style in Styles)
        {
            if (style is not FluentTheme fluent)
                continue;

            fluent.Palettes[ThemeVariant.Light] = new ColorPaletteResources
            {
                Accent = Color.FromUInt32(0xFF5C4D3A),
            };
            fluent.Palettes[ThemeVariant.Dark] = new ColorPaletteResources
            {
                Accent = Color.FromUInt32(0xFFE2E6ED),
            };
            // FluentTheme.Palettes only accepts Light and Dark (Avalonia 12). Theme "Iu5" uses
            // App.Resources ThemeDictionary (Iu5Theme.axaml); accent there is AccentBrush #00E5FF.
            break;
        }
    }

    private static async Task SafeInitialize(MainWindowViewModel vm)
    {
        try
        {
            await vm.InitializeAsync();
        }
        catch (Exception ex)
        {
            vm.Notifications.Push("Ошибка инициализации", ex.Message, NotificationKind.Error);
        }
    }

    private static void HookGlobalExceptionHandlers(NotificationService notifications)
    {
        AppDomain.CurrentDomain.UnhandledException += (_, e) =>
        {
            if (e.ExceptionObject is Exception ex)
            {
                notifications.Push("Непредвиденная ошибка", ex.Message, NotificationKind.Error);
            }
        };

        TaskScheduler.UnobservedTaskException += (_, e) =>
        {
            notifications.Push("Фоновая ошибка", e.Exception.Message, NotificationKind.Warning);
            e.SetObserved();
        };

        Dispatcher.UIThread.UnhandledException += (_, e) =>
        {
            notifications.Push("Ошибка интерфейса", e.Exception.Message, NotificationKind.Error);
            e.Handled = true;
        };
    }
}
