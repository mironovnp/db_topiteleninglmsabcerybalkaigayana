using System.Collections.Generic;
using System.Text.Json.Serialization;

namespace CaseChampGui.Models;

public sealed class MistralKeyPreferences
{
    [JsonPropertyName("users")]
    public Dictionary<string, MistralUserKeyPreferences> Users { get; set; } = new();

    [JsonPropertyName("shared_owner")]
    public string? SharedOwner { get; set; }

    [JsonPropertyName("has_organization_key")]
    public bool HasOrganizationKey { get; set; }

    [JsonPropertyName("policy")]
    public MistralKeyPolicy Policy { get; set; } = new();
}

public sealed class MistralKeyPolicy
{
    [JsonPropertyName("preset")]
    public MistralKeyPolicyPreset Preset { get; set; } = MistralKeyPolicyPreset.UserChoice;
}

public sealed class MistralUserKeyPreferences
{
    [JsonPropertyName("share_with_others_on_pc")]
    public bool ShareWithOthersOnPc { get; set; }

    [JsonPropertyName("use_shared_fallback")]
    public bool UseSharedFallback { get; set; } = true;

    [JsonPropertyName("shared_notice_acknowledged")]
    public bool SharedNoticeAcknowledged { get; set; }

    [JsonPropertyName("selection_mode")]
    public MistralKeySelectionMode SelectionMode { get; set; } = MistralKeySelectionMode.Auto;

    [JsonPropertyName("personal_provided_by_admin")]
    public bool PersonalProvidedByAdmin { get; set; }
}
