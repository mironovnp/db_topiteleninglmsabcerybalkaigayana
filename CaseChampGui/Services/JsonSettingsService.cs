using System;
using System.IO;
using System.Text.Json;
using System.Threading.Tasks;
using Avalonia.Threading;
using CaseChampGui.Models;

namespace CaseChampGui.Services;

public sealed class JsonSettingsService : ISettingsService
{
    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        WriteIndented = true,
        PropertyNameCaseInsensitive = true,
    };

    private readonly string _filePath;
    private AppSettings _settings = new();

    public JsonSettingsService()
    {
        var baseDir = Environment.GetFolderPath(
            Environment.SpecialFolder.ApplicationData,
            Environment.SpecialFolderOption.Create);
        if (string.IsNullOrEmpty(baseDir))
        {
            baseDir = Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.UserProfile),
                ".config");
        }
        var dir = Path.Combine(baseDir, "CaseChamp");
        Directory.CreateDirectory(dir);
        _filePath = Path.Combine(dir, "settings.json");
    }

    public AppSettings Current => _settings;

    public event EventHandler<AppSettings>? SettingsChanged;

    public void Load()
    {
        try
        {
            if (!File.Exists(_filePath))
            {
                _settings = new AppSettings();
                return;
            }
            using var stream = File.OpenRead(_filePath);
            var loaded = JsonSerializer.Deserialize<AppSettings>(stream, JsonOptions);
            _settings = loaded ?? new AppSettings();
        }
        catch
        {
            _settings = new AppSettings();
        }
    }

    public Task LoadAsync()
    {
        Load();
        return Task.CompletedTask;
    }

    public async Task SaveAsync(AppSettings settings)
    {
        _settings = settings.Clone();
        try
        {
            await using var stream = File.Create(_filePath);
            await JsonSerializer.SerializeAsync(stream, _settings, JsonOptions);
        }
        catch
        {
        }

        var snapshot = _settings;
        var handler = SettingsChanged;
        if (handler is null) return;

        void Raise() => handler(this, snapshot);

        if (Dispatcher.UIThread.CheckAccess())
            Raise();
        else
            Dispatcher.UIThread.Post(Raise);
    }
}
