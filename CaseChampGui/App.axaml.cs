using System;
using System.Threading.Tasks;
using Avalonia;
using Avalonia.Controls;
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
    public override void Initialize()
    {
        AvaloniaXamlLoader.Load(this);
    }

    public override void OnFrameworkInitializationCompleted()
    {
        ApplyFluentAccentPalettes();

        if (ApplicationLifetime is IClassicDesktopStyleApplicationLifetime desktop)
        {
            desktop.ShutdownMode = ShutdownMode.OnMainWindowClose;

            var settingsService = new JsonSettingsService();
            settingsService.Load();
            var themeService = new ThemeService();
            themeService.Apply(settingsService.Current.Theme);

            var loading = new LoadingWindow();
            desktop.MainWindow = loading;
            base.OnFrameworkInitializationCompleted();

            loading.Show();
            loading.Activate();

            _ = Dispatcher.UIThread.InvokeAsync(() => OpenMainWindowAsync(desktop, loading, settingsService, themeService));
        }
        else
        {
            base.OnFrameworkInitializationCompleted();
        }
    }

    private static async Task OpenMainWindowAsync(
        IClassicDesktopStyleApplicationLifetime desktop,
        Window loading,
        JsonSettingsService settingsService,
        ThemeService themeService)
    {
        await Dispatcher.UIThread.InvokeAsync(() => { }, DispatcherPriority.Render);

        MainWindow? mainWindow = null;
        MainWindowViewModel? mainVm = null;
        LocalServerService? localServer = null;
        HttpDatabaseClient? client = null;

        try
        {
            client = new HttpDatabaseClient();
            var mistralApiKeys = new MistralApiKeyStore();
            void SyncMistralUser()
            {
                mistralApiKeys.SetCurrentUser(client.CurrentUser, client.IsGlobalAdmin);
            }
            client.StateChanged += (_, _) => Dispatcher.UIThread.Post(SyncMistralUser);
            var notifications = new NotificationService();
            localServer = new LocalServerService();
            var schemaService = new SchemaService(client);
            var schemaPane = new SchemaPaneViewModel(schemaService);
            var text2Sql = new MistralText2SqlService(mistralApiKeys, schemaService);
            var columnFilterTranslator = new MistralColumnFilterService(mistralApiKeys, schemaService);

            var sqlVm = new SqlViewModel(client, text2Sql, mistralApiKeys, schemaPane, notifications);
            var text2SqlVm = new Text2SqlViewModel(text2Sql, mistralApiKeys, client, schemaPane, notifications);
            var browseVm = new TableBrowseViewModel(client, schemaService, columnFilterTranslator, notifications);
            var settingsVm = new SettingsViewModel(settingsService, themeService, client, mistralApiKeys);

            mainVm = new MainWindowViewModel(
                client, settingsService, themeService, schemaService, localServer, notifications,
                sqlVm, text2SqlVm, browseVm, settingsVm, schemaPane,
                () => mainVm!.CompleteAuthenticationGateAsync());

            HookGlobalExceptionHandlers(notifications);

            SyncMistralUser();

            mainWindow = new MainWindow { DataContext = mainVm };
            desktop.MainWindow = mainWindow;

            var shutdownRequested = false;
            void ShutdownApp()
            {
                if (shutdownRequested) return;
                shutdownRequested = true;

                try
                {
                    mainVm.Dispose();
                    localServer.Dispose();
                    client.Dispose();
                    text2Sql.Dispose();
                    columnFilterTranslator.Dispose();
                }
                catch
                {
                }

                desktop.Shutdown();
            }

            desktop.Exit += (_, _) => ShutdownApp();
            mainWindow.Closed += (_, _) => ShutdownApp();

            mainWindow.Show();
            mainWindow.Activate();
            mainWindow.WindowState = WindowState.Normal;

            _ = SafeInitialize(mainVm);
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"[CaseChamp GUI] {ex}");
            if (mainWindow is null)
            {
                var error = new Window
                {
                    Title = "СУБД Топит_Еленинг — ошибка",
                    Width = 520,
                    Height = 200,
                    WindowStartupLocation = WindowStartupLocation.CenterScreen,
                    Background = new SolidColorBrush(Color.Parse("#F2EDE4")),
                    Content = new TextBlock
                    {
                        Text = ex.Message,
                        TextWrapping = TextWrapping.Wrap,
                        Margin = new Thickness(24),
                        Foreground = new SolidColorBrush(Color.Parse("#A33A3A")),
                    },
                };
                desktop.MainWindow = error;
                error.Show();
                error.Activate();
            }
        }
        finally
        {
            loading.Close();
        }

        await Task.CompletedTask;
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
            break;
        }
    }

    private static async Task SafeInitialize(MainWindowViewModel vm)
    {
        try
        {
            await vm.InitializeAsync();
        }
        catch (OperationCanceledException)
        {
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
