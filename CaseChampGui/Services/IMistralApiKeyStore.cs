using System;
using System.Collections.Generic;
using CaseChampGui.Models;

namespace CaseChampGui.Services;

public enum MistralKeySource
{
    None,
    Environment,
    Personal,
    Shared,
    Organization,
    Legacy,
}

public interface IMistralApiKeyStore
{
    string? CurrentUsername { get; }

    bool HasValidKey { get; }

    bool HasPersonalKey { get; }

    bool IsPersonalKeyFromAdmin { get; }

    bool IsUsingSharedKey { get; }

    bool IsUsingOrganizationKey { get; }

    string? SharedKeyOwner { get; }

    MistralKeySource ActiveKeySource { get; }

    MistralKeySelectionMode SelectionMode { get; set; }

    bool IsKeyChoiceLocked { get; }

    string KeyChoiceLockHint { get; }

    string ActiveKeyBadge { get; }

    bool ShouldShowSharedKeyNotice { get; }

    IReadOnlyList<MistralKeyChoiceItem> GetKeyChoices();

    string? GetApiKey();

    string PersonalKeyFilePath { get; }

    void SetCurrentUser(string? username, bool isGlobalAdmin);

    void SavePersonalApiKey(string apiKey);

    bool SharePersonalKeyWithOthers { get; set; }

    bool UseSharedKeyFallback { get; set; }

    void AcknowledgeSharedKeyNotice();

    MistralKeyPolicyPreset KeyPolicyPreset { get; set; }

    void SaveOrganizationApiKey(string apiKey);

    void SaveApiKeyForUser(string targetUsername, string apiKey);

    void Reload();

    event EventHandler? ApiKeyChanged;
}
