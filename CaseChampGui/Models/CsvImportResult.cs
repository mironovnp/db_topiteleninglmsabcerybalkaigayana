namespace CaseChampGui.Models;

public sealed class CsvImportResult
{
    public bool Success { get; init; }
    public string Message { get; init; } = string.Empty;
    public string? ServerFileName { get; init; }
    public string? Sql { get; init; }
    public int AffectedRows { get; init; }
}
