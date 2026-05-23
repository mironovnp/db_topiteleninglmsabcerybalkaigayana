using System.Text.Json.Serialization;

namespace CaseChampGui.Models;

public enum AppTheme
{
    System,
    Light,
    Dark,
    Iu5,
}

public sealed class AppSettings
{
    [JsonPropertyName("theme")]
    public AppTheme Theme { get; set; } = AppTheme.System;

    [JsonPropertyName("host")]
    public string Host { get; set; } = "127.0.0.1";

    [JsonPropertyName("port")]
    public int Port { get; set; } = 8080;

    [JsonPropertyName("auto_connect")]
    public bool AutoConnect { get; set; } = true;

    [JsonPropertyName("editor_font_size")]
    public double EditorFontSize { get; set; } = 14;

    [JsonPropertyName("animations_enabled")]
    public bool AnimationsEnabled { get; set; } = true;

    [JsonPropertyName("compact_mode")]
    public bool CompactMode { get; set; }

    [JsonPropertyName("chat_mode_enabled")]
    public bool ChatModeEnabled { get; set; }

    [JsonPropertyName("auto_start_local_server")]
    public bool AutoStartLocalServer { get; set; } = true;

    [JsonPropertyName("sidebar_auto_collapse")]
    public bool SidebarAutoCollapse { get; set; } = true;

    [JsonPropertyName("intellisense_enabled")]
    public bool IntellisenseEnabled { get; set; } = true;

    /// <summary>Если true, первый Backspace при видимой подсказке только скрывает её, не удаляя текст.</summary>
    [JsonPropertyName("intellisense_backspace_dismisses_ghost")]
    public bool IntellisenseBackspaceDismissesGhost { get; set; } = true;

    [JsonPropertyName("auth_completed_once")]
    public bool AuthCompletedOnce { get; set; }

    [JsonPropertyName("last_username")]
    public string? LastUsername { get; set; }

    [JsonPropertyName("remember_password")]
    public bool RememberPassword { get; set; }

    [JsonPropertyName("encrypted_password")]
    public string? EncryptedPassword { get; set; }

    [JsonPropertyName("display_name_override")]
    public string? DisplayNameOverride { get; set; }

    [JsonPropertyName("avatar_file_path")]
    public string? AvatarFilePath { get; set; }

    public AppSettings Clone() => new()
    {
        Theme = Theme,
        Host = Host,
        Port = Port,
        AutoConnect = AutoConnect,
        EditorFontSize = EditorFontSize,
        AnimationsEnabled = AnimationsEnabled,
        CompactMode = CompactMode,
        ChatModeEnabled = ChatModeEnabled,
        AutoStartLocalServer = AutoStartLocalServer,
        SidebarAutoCollapse = SidebarAutoCollapse,
        IntellisenseEnabled = IntellisenseEnabled,
        IntellisenseBackspaceDismissesGhost = IntellisenseBackspaceDismissesGhost,
        AuthCompletedOnce = AuthCompletedOnce,
        LastUsername = LastUsername,
        RememberPassword = RememberPassword,
        EncryptedPassword = EncryptedPassword,
        DisplayNameOverride = DisplayNameOverride,
        AvatarFilePath = AvatarFilePath,
    };
}
