using System.Text.Json.Serialization;

namespace CaseChampGui.Models;

public sealed class QueryRequest
{
    [JsonPropertyName("sql")]
    public string Sql { get; set; } = string.Empty;

    [JsonPropertyName("dry_run")]
    public bool DryRun { get; set; }

    [JsonPropertyName("current_db")]
    public string CurrentDb { get; set; } = string.Empty;

    [JsonPropertyName("current_user")]
    public string CurrentUser { get; set; } = string.Empty;

    [JsonPropertyName("session_id")]
    public string? SessionId { get; set; }
}
