using System;
using System.Threading.Tasks;
using CaseChampGui.Models;

namespace CaseChampGui.Services;

public interface ISettingsService
{
    AppSettings Current { get; }
    event EventHandler<AppSettings>? SettingsChanged;
    void Load();
    Task LoadAsync();
    Task SaveAsync(AppSettings settings);
}
