using CaseChampGui.Services;

namespace CaseChampGui.Models;

public sealed class MistralKeyChoiceItem
{
    public MistralKeySelectionMode Mode { get; init; }

    public string Title { get; init; } = string.Empty;

    public string Description { get; init; } = string.Empty;

    public bool IsAvailable { get; init; }

    public bool IsActive { get; init; }

    public bool IsAdminProvided { get; init; }

    public bool IsSelectable { get; init; }

    public MistralKeySource? MapsToSource { get; init; }
}
