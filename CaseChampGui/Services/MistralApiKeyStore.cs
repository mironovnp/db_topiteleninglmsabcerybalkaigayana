using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.Json;
using CaseChampGui.Models;

namespace CaseChampGui.Services;

public sealed class MistralApiKeyStore : IMistralApiKeyStore
{
    private const string Placeholder = "put-your-mistral-api-key-here";
    private const string LegacyFileName = "mistral_api_key";
    private const string SharedFileName = "_shared.key";
    private const string OrganizationFileName = "_organization.key";
    private const string PrefsFileName = "mistral_prefs.json";

    private static readonly JsonSerializerOptions JsonOptions = new() { WriteIndented = true };

    private readonly string _configDir;
    private readonly string _keysDir;
    private readonly string _prefsPath;

    private string? _currentUsername;
    private bool _isGlobalAdmin;
    private string? _cachedKey;
    private MistralKeySource _activeSource = MistralKeySource.None;
    private string? _sharedOwner;
    private MistralKeyPreferences _prefs = new();

    public MistralApiKeyStore()
    {
        _configDir = GetUserConfigDirectory();
        _keysDir = Path.Combine(_configDir, "keys");
        _prefsPath = Path.Combine(_configDir, PrefsFileName);
        Directory.CreateDirectory(_keysDir);
        LoadPrefs();
        Reload();
    }

    public string? CurrentUsername => _currentUsername;

    public bool HasValidKey => !string.IsNullOrEmpty(GetApiKey());

    public bool HasPersonalKey =>
        !string.IsNullOrEmpty(_currentUsername) && HasKeyInFile(PersonalKeyPath(_currentUsername));

    public bool IsPersonalKeyFromAdmin =>
        !string.IsNullOrEmpty(_currentUsername) && GetUserPrefs(_currentUsername).PersonalProvidedByAdmin;

    public bool IsUsingSharedKey => _activeSource == MistralKeySource.Shared;

    public bool IsUsingOrganizationKey => _activeSource == MistralKeySource.Organization;

    public string? SharedKeyOwner => _sharedOwner;

    public MistralKeySource ActiveKeySource => _activeSource;

    public bool IsKeyChoiceLocked => _prefs.Policy.Preset != MistralKeyPolicyPreset.UserChoice;

    public string KeyChoiceLockHint => _prefs.Policy.Preset switch
    {
        MistralKeyPolicyPreset.ForceOrganization =>
            "Администратор зафиксировал: для Text2SQL используется только ключ организации.",
        MistralKeyPolicyPreset.ForcePersonal =>
            "Администратор зафиксировал: для Text2SQL можно использовать только личный ключ.",
        MistralKeyPolicyPreset.ForceSharedPc =>
            "Администратор зафиксировал: для Text2SQL используется только общий ключ с этого компьютера.",
        _ => string.Empty,
    };

    public string ActiveKeyBadge => BuildActiveKeyBadge();

    public bool ShouldShowSharedKeyNotice =>
        IsUsingSharedKey
        && !IsKeyChoiceLocked
        && !string.IsNullOrEmpty(_currentUsername)
        && !GetUserPrefs(_currentUsername).SharedNoticeAcknowledged;

    public string PersonalKeyFilePath =>
        string.IsNullOrEmpty(_currentUsername)
            ? Path.Combine(_keysDir, "personal.key")
            : PersonalKeyPath(_currentUsername);

    public event EventHandler? ApiKeyChanged;

    public MistralKeySelectionMode SelectionMode
    {
        get
        {
            if (IsKeyChoiceLocked)
                return PolicyPresetToMode(_prefs.Policy.Preset);
            return string.IsNullOrEmpty(_currentUsername)
                ? MistralKeySelectionMode.Auto
                : GetUserPrefs(_currentUsername).SelectionMode;
        }
        set
        {
            if (IsKeyChoiceLocked || string.IsNullOrEmpty(_currentUsername)) return;
            var user = GetUserPrefs(_currentUsername);
            if (user.SelectionMode == value) return;
            user.SelectionMode = value;
            SavePrefs();
            ReloadAndNotify();
        }
    }

    public MistralKeyPolicyPreset KeyPolicyPreset
    {
        get => _prefs.Policy.Preset;
        set
        {
            if (!_isGlobalAdmin) return;
            if (_prefs.Policy.Preset == value) return;
            _prefs.Policy.Preset = value;
            SavePrefs();
            ReloadAndNotify();
        }
    }

    public bool SharePersonalKeyWithOthers
    {
        get => !string.IsNullOrEmpty(_currentUsername) && GetUserPrefs(_currentUsername).ShareWithOthersOnPc;
        set
        {
            if (IsKeyChoiceLocked || string.IsNullOrEmpty(_currentUsername)) return;
            var user = GetUserPrefs(_currentUsername);
            if (user.ShareWithOthersOnPc == value) return;
            user.ShareWithOthersOnPc = value;
            SavePrefs();
            ApplySharingFromCurrentUser();
            ReloadAndNotify();
        }
    }

    public bool UseSharedKeyFallback
    {
        get => string.IsNullOrEmpty(_currentUsername) || GetUserPrefs(_currentUsername).UseSharedFallback;
        set
        {
            if (IsKeyChoiceLocked || string.IsNullOrEmpty(_currentUsername)) return;
            var user = GetUserPrefs(_currentUsername);
            if (user.UseSharedFallback == value) return;
            user.UseSharedFallback = value;
            SavePrefs();
            ReloadAndNotify();
        }
    }

    public void SetCurrentUser(string? username, bool isGlobalAdmin)
    {
        _currentUsername = string.IsNullOrWhiteSpace(username) ? null : username.Trim();
        _isGlobalAdmin = isGlobalAdmin;
        MigrateLegacyKeyIfNeeded();
        ReloadAndNotify();
    }

    public IReadOnlyList<MistralKeyChoiceItem> GetKeyChoices()
    {
        var choices = new List<MistralKeyChoiceItem>();
        var effective = SelectionMode;
        var sharedAvailable = IsSharedPcAvailable();
        var orgAvailable = IsOrganizationAvailable();
        var personalAvailable = HasPersonalKey;

        choices.Add(new MistralKeyChoiceItem
        {
            Mode = MistralKeySelectionMode.Auto,
            Title = "Автоматически",
            Description = "Личный → организация → общий с ПК → унаследованный",
            IsAvailable = !IsKeyChoiceLocked || _prefs.Policy.Preset == MistralKeyPolicyPreset.UserChoice,
            IsActive = effective == MistralKeySelectionMode.Auto,
            IsSelectable = !IsKeyChoiceLocked,
            MapsToSource = null,
        });

        choices.Add(new MistralKeyChoiceItem
        {
            Mode = MistralKeySelectionMode.Personal,
            Title = personalAvailable
                ? (IsPersonalKeyFromAdmin ? "Личный ключ · задан администратором" : "Личный ключ")
                : "Личный ключ (не задан)",
            Description = personalAvailable
                ? "Ваш сохранённый ключ Mistral на этом компьютере"
                : "Введите и сохраните ключ ниже",
            IsAvailable = personalAvailable || !IsKeyChoiceLocked,
            IsActive = effective == MistralKeySelectionMode.Personal,
            IsAdminProvided = IsPersonalKeyFromAdmin,
            IsSelectable = !IsKeyChoiceLocked && _prefs.Policy.Preset != MistralKeyPolicyPreset.ForceOrganization,
            MapsToSource = MistralKeySource.Personal,
        });

        choices.Add(new MistralKeyChoiceItem
        {
            Mode = MistralKeySelectionMode.Organization,
            Title = orgAvailable
                ? "Ключ организации · задан администратором"
                : "Ключ организации (не задан)",
            Description = orgAvailable
                ? "Общий ключ для пользователей без личного"
                : "Попросите администратора задать ключ организации",
            IsAvailable = orgAvailable,
            IsActive = effective == MistralKeySelectionMode.Organization,
            IsAdminProvided = true,
            IsSelectable = !IsKeyChoiceLocked && _prefs.Policy.Preset != MistralKeyPolicyPreset.ForcePersonal,
            MapsToSource = MistralKeySource.Organization,
        });

        var sharedTitle = sharedAvailable
            ? $"Общий ключ · пользователь «{_prefs.SharedOwner}»"
            : "Общий ключ с ПК (недоступен)";
        choices.Add(new MistralKeyChoiceItem
        {
            Mode = MistralKeySelectionMode.SharedPc,
            Title = sharedTitle,
            Description = sharedAvailable
                ? "Ключ, которым другой пользователь поделился на этом компьютере"
                : "Никто не поделился ключом или вы отключили использование общего",
            IsAvailable = sharedAvailable,
            IsActive = effective == MistralKeySelectionMode.SharedPc,
            IsAdminProvided = false,
            IsSelectable = !IsKeyChoiceLocked && _prefs.Policy.Preset != MistralKeyPolicyPreset.ForcePersonal,
            MapsToSource = MistralKeySource.Shared,
        });

        if (TryEnvironmentKey(out _))
        {
            choices.Add(new MistralKeyChoiceItem
            {
                Mode = MistralKeySelectionMode.Auto,
                Title = "Переменная окружения MISTRAL_API_KEY",
                Description = "Имеет приоритет в режиме «Автоматически»",
                IsAvailable = true,
                IsActive = _activeSource == MistralKeySource.Environment,
                IsSelectable = false,
                MapsToSource = MistralKeySource.Environment,
            });
        }

        return choices;
    }

    public string? GetApiKey()
    {
        if (_cachedKey is not null && _activeSource != MistralKeySource.None)
            return _cachedKey;
        Reload();
        return _cachedKey;
    }

    public void Reload() => (_cachedKey, _activeSource, _sharedOwner) = ResolveKey();

    public void SavePersonalApiKey(string apiKey)
    {
        if (string.IsNullOrEmpty(_currentUsername))
            throw new InvalidOperationException("Сначала войдите в учётную запись CaseChamp.");

        var trimmed = NormalizeKey(apiKey);
        WriteKeyFile(PersonalKeyPath(_currentUsername), trimmed);

        var user = GetUserPrefs(_currentUsername);
        user.PersonalProvidedByAdmin = false;
        SavePrefs();

        if (SharePersonalKeyWithOthers)
            ApplySharingFromCurrentUser();

        ReloadAndNotify();
    }

    public void AcknowledgeSharedKeyNotice()
    {
        if (string.IsNullOrEmpty(_currentUsername)) return;
        var user = GetUserPrefs(_currentUsername);
        if (user.SharedNoticeAcknowledged) return;
        user.SharedNoticeAcknowledged = true;
        SavePrefs();
        ApiKeyChanged?.Invoke(this, EventArgs.Empty);
    }

    public void SaveOrganizationApiKey(string apiKey)
    {
        if (!_isGlobalAdmin)
            throw new InvalidOperationException("Только администратор может задать общий ключ организации.");

        var trimmed = NormalizeKey(apiKey);
        WriteKeyFile(OrganizationKeyPath(), trimmed);
        _prefs.HasOrganizationKey = true;
        SavePrefs();
        ReloadAndNotify();
    }

    public void SaveApiKeyForUser(string targetUsername, string apiKey)
    {
        if (!_isGlobalAdmin)
            throw new InvalidOperationException("Только администратор может задавать ключи другим пользователям.");

        var user = (targetUsername ?? string.Empty).Trim();
        if (string.IsNullOrEmpty(user))
            throw new ArgumentException("Укажите имя пользователя CaseChamp.", nameof(targetUsername));

        var trimmed = NormalizeKey(apiKey);
        WriteKeyFile(PersonalKeyPath(user), trimmed);

        var userPrefs = GetUserPrefs(user);
        userPrefs.PersonalProvidedByAdmin = true;
        SavePrefs();
        ReloadAndNotify();
    }

    private string BuildActiveKeyBadge() => _activeSource switch
    {
        MistralKeySource.Personal when IsPersonalKeyFromAdmin => "Сейчас: личный ключ · задан администратором",
        MistralKeySource.Personal => "Сейчас: личный ключ",
        MistralKeySource.Organization => "Сейчас: ключ организации · задан администратором",
        MistralKeySource.Shared => $"Сейчас: общий ключ · {_sharedOwner}",
        MistralKeySource.Environment => "Сейчас: переменная окружения MISTRAL_API_KEY",
        MistralKeySource.Legacy => "Сейчас: унаследованный локальный ключ",
        _ => "Сейчас: ключ не выбран",
    };

    private (string? Key, MistralKeySource Source, string? SharedOwner) ResolveKey()
    {
        if (TryEnvironmentKey(out var env) && SelectionMode == MistralKeySelectionMode.Auto)
            return (env, MistralKeySource.Environment, null);

        return SelectionMode switch
        {
            MistralKeySelectionMode.Personal => TryPersonal(),
            MistralKeySelectionMode.Organization => TryOrganization(),
            MistralKeySelectionMode.SharedPc => TryShared(),
            _ => ResolveAuto(),
        };
    }

    private (string? Key, MistralKeySource Source, string? SharedOwner) ResolveAuto()
    {
        if (TryEnvironmentKey(out var env))
            return (env, MistralKeySource.Environment, null);

        var personal = TryPersonal();
        if (personal.Key is not null) return personal;

        var org = TryOrganization();
        if (org.Key is not null) return org;

        if (UseSharedKeyFallback || IsKeyChoiceLocked && _prefs.Policy.Preset == MistralKeyPolicyPreset.ForceSharedPc)
        {
            var shared = TryShared();
            if (shared.Key is not null) return shared;
        }

        var projectRoot = TryFindProjectRoot();
        if (projectRoot is not null)
        {
            var projectKey = ReadKeyFile(Path.Combine(projectRoot, LegacyFileName));
            if (!IsPlaceholder(projectKey))
                return (projectKey, MistralKeySource.Legacy, null);
        }

        var legacyUser = ReadKeyFile(Path.Combine(_configDir, LegacyFileName));
        if (!IsPlaceholder(legacyUser))
            return (legacyUser, MistralKeySource.Legacy, null);

        return (null, MistralKeySource.None, null);
    }

    private (string? Key, MistralKeySource Source, string? SharedOwner) TryPersonal()
    {
        if (string.IsNullOrEmpty(_currentUsername))
            return (null, MistralKeySource.None, null);
        var personal = ReadKeyFile(PersonalKeyPath(_currentUsername));
        return IsPlaceholder(personal)
            ? (null, MistralKeySource.None, null)
            : (personal, MistralKeySource.Personal, null);
    }

    private (string? Key, MistralKeySource Source, string? SharedOwner) TryOrganization()
    {
        if (!_prefs.HasOrganizationKey)
            return (null, MistralKeySource.None, null);
        var org = ReadKeyFile(OrganizationKeyPath());
        return IsPlaceholder(org)
            ? (null, MistralKeySource.None, null)
            : (org, MistralKeySource.Organization, null);
    }

    private (string? Key, MistralKeySource Source, string? SharedOwner) TryShared()
    {
        if (!IsSharedPcAvailable())
            return (null, MistralKeySource.None, null);
        var shared = ReadKeyFile(SharedKeyPath());
        return IsPlaceholder(shared)
            ? (null, MistralKeySource.None, null)
            : (shared, MistralKeySource.Shared, _prefs.SharedOwner);
    }

    private bool IsOrganizationAvailable() =>
        _prefs.HasOrganizationKey && !IsPlaceholder(ReadKeyFile(OrganizationKeyPath()));

    private bool IsSharedPcAvailable()
    {
        if (string.IsNullOrEmpty(_prefs.SharedOwner)) return false;
        if (IsPlaceholder(ReadKeyFile(SharedKeyPath()))) return false;
        return UseSharedKeyFallback || _prefs.Policy.Preset == MistralKeyPolicyPreset.ForceSharedPc;
    }

    private static bool TryEnvironmentKey(out string? key)
    {
        key = Environment.GetEnvironmentVariable("MISTRAL_API_KEY");
        if (!IsPlaceholder(key)) return true;

        var envFile = Environment.GetEnvironmentVariable("MISTRAL_API_KEY_FILE");
        if (!string.IsNullOrWhiteSpace(envFile))
        {
            key = ReadKeyFile(envFile);
            if (!IsPlaceholder(key)) return true;
        }

        key = null;
        return false;
    }

    private static MistralKeySelectionMode PolicyPresetToMode(MistralKeyPolicyPreset preset) =>
        preset switch
        {
            MistralKeyPolicyPreset.ForceOrganization => MistralKeySelectionMode.Organization,
            MistralKeyPolicyPreset.ForcePersonal => MistralKeySelectionMode.Personal,
            MistralKeyPolicyPreset.ForceSharedPc => MistralKeySelectionMode.SharedPc,
            _ => MistralKeySelectionMode.Auto,
        };

    private void ApplySharingFromCurrentUser()
    {
        if (string.IsNullOrEmpty(_currentUsername)) return;

        var userPrefs = GetUserPrefs(_currentUsername);
        if (!userPrefs.ShareWithOthersOnPc)
        {
            if (string.Equals(_prefs.SharedOwner, _currentUsername, StringComparison.OrdinalIgnoreCase))
            {
                TryDeleteFile(SharedKeyPath());
                _prefs.SharedOwner = null;
                SavePrefs();
            }
            return;
        }

        var personal = ReadKeyFile(PersonalKeyPath(_currentUsername));
        if (IsPlaceholder(personal)) return;

        WriteKeyFile(SharedKeyPath(), personal!);
        _prefs.SharedOwner = _currentUsername;
        SavePrefs();
    }

    private void MigrateLegacyKeyIfNeeded()
    {
        if (string.IsNullOrEmpty(_currentUsername)) return;
        if (HasKeyInFile(PersonalKeyPath(_currentUsername))) return;

        var legacyPaths = new[]
        {
            Path.Combine(_configDir, LegacyFileName),
            Path.Combine(TryFindProjectRoot() ?? string.Empty, LegacyFileName),
        };

        foreach (var path in legacyPaths)
        {
            if (string.IsNullOrEmpty(path) || !File.Exists(path)) continue;
            var key = ReadKeyFile(path);
            if (IsPlaceholder(key)) continue;
            WriteKeyFile(PersonalKeyPath(_currentUsername), key!);
            return;
        }
    }

    private MistralUserKeyPreferences GetUserPrefs(string username)
    {
        if (!_prefs.Users.TryGetValue(username, out var prefs))
        {
            prefs = new MistralUserKeyPreferences();
            _prefs.Users[username] = prefs;
        }
        return prefs;
    }

    private void LoadPrefs()
    {
        try
        {
            if (!File.Exists(_prefsPath)) return;
            var json = File.ReadAllText(_prefsPath);
            var loaded = JsonSerializer.Deserialize<MistralKeyPreferences>(json, JsonOptions);
            if (loaded is not null) _prefs = loaded;
        }
        catch
        {
            _prefs = new MistralKeyPreferences();
        }

        _prefs.Policy ??= new MistralKeyPolicy();
        _prefs.Users ??= new Dictionary<string, MistralUserKeyPreferences>();
    }

    private void SavePrefs()
    {
        try
        {
            File.WriteAllText(_prefsPath, JsonSerializer.Serialize(_prefs, JsonOptions));
        }
        catch
        {
        }
    }

    private void ReloadAndNotify()
    {
        Reload();
        ApiKeyChanged?.Invoke(this, EventArgs.Empty);
    }

    private string PersonalKeyPath(string username) =>
        Path.Combine(_keysDir, SanitizeUsername(username) + ".key");

    private static string SharedKeyPath() =>
        Path.Combine(GetUserConfigDirectory(), "keys", SharedFileName);

    private static string OrganizationKeyPath() =>
        Path.Combine(GetUserConfigDirectory(), "keys", OrganizationFileName);

    private static string SanitizeUsername(string username)
    {
        var chars = username.Where(c => char.IsLetterOrDigit(c) || c == '_').ToArray();
        return chars.Length == 0 ? "user" : new string(chars);
    }

    private static string NormalizeKey(string apiKey)
    {
        var trimmed = (apiKey ?? string.Empty).Trim();
        if (IsPlaceholder(trimmed) || string.IsNullOrEmpty(trimmed))
            throw new ArgumentException("Укажите действительный API-ключ Mistral.", nameof(apiKey));
        return trimmed;
    }

    private static bool HasKeyInFile(string path)
    {
        var key = ReadKeyFile(path);
        return !IsPlaceholder(key);
    }

    private static void WriteKeyFile(string path, string key)
    {
        var dir = Path.GetDirectoryName(path);
        if (!string.IsNullOrEmpty(dir))
            Directory.CreateDirectory(dir);
        File.WriteAllText(path, key + Environment.NewLine);
    }

    private static string? ReadKeyFile(string path)
    {
        try
        {
            if (!File.Exists(path)) return null;
            var line = File.ReadLines(path).FirstOrDefault();
            return string.IsNullOrWhiteSpace(line) ? null : line.Trim();
        }
        catch
        {
            return null;
        }
    }

    private static void TryDeleteFile(string path)
    {
        try
        {
            if (File.Exists(path)) File.Delete(path);
        }
        catch
        {
        }
    }

    private static bool IsPlaceholder(string? key)
    {
        if (string.IsNullOrWhiteSpace(key)) return true;
        var trimmed = key.Trim();
        return string.Equals(trimmed, Placeholder, StringComparison.Ordinal)
               || trimmed.Contains("PASTE_", StringComparison.Ordinal)
               || trimmed.Contains("YOUR_", StringComparison.Ordinal);
    }

    private static string GetUserConfigDirectory()
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

        return Path.Combine(baseDir, "CaseChamp");
    }

    public static string? TryFindProjectRoot()
    {
        try
        {
            var dir = new DirectoryInfo(AppContext.BaseDirectory);
            for (var depth = 0; depth < 10 && dir is not null; depth++, dir = dir.Parent)
            {
                if (File.Exists(Path.Combine(dir.FullName, "CMakeLists.txt"))
                    || File.Exists(Path.Combine(dir.FullName, "run.sh")))
                {
                    return dir.FullName;
                }
            }
        }
        catch
        {
        }

        return null;
    }
}
