using System.Collections.Generic;
using System.Text.Json.Serialization;

namespace CaseChampGui.Models;

public sealed class QueryResult
{
    [JsonPropertyName("success")]
    public bool Success { get; set; }

    [JsonPropertyName("message")]
    public string Message { get; set; } = string.Empty;

    [JsonPropertyName("type")]
    public string Type { get; set; } = string.Empty;

    [JsonPropertyName("affected_rows")]
    public int AffectedRows { get; set; }

    [JsonPropertyName("columns")]
    public List<string> Columns { get; set; } = new();

    [JsonPropertyName("rows")]
    public List<List<string>> Rows { get; set; } = new();

    [JsonPropertyName("current_db")]
    public string? CurrentDb { get; set; }

    [JsonPropertyName("current_user")]
    public string? CurrentUser { get; set; }

    [JsonPropertyName("is_admin")]
    public bool IsAdmin { get; set; }

    [JsonPropertyName("session_id")]
    public string? SessionId { get; set; }

    public static QueryResult Fail(string message) => new()
    {
        Success = false,
        Message = message,
    };
}
