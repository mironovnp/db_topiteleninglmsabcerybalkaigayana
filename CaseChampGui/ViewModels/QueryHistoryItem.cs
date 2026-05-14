using System;

namespace CaseChampGui.ViewModels;

public sealed class QueryHistoryItem
{
    public string Sql { get; }
    public DateTime Timestamp { get; }
    public bool Success { get; }
    public string Summary { get; }

    public QueryHistoryItem(string sql, bool success, string summary)
    {
        Sql = sql;
        Timestamp = DateTime.Now;
        Success = success;
        Summary = summary;
    }

    public string Preview
    {
        get
        {
            var trimmed = Sql.Replace('\n', ' ').Replace('\r', ' ').Trim();
            return trimmed.Length > 100 ? trimmed[..100] + "…" : trimmed;
        }
    }

    public string StatusGlyph => Success ? "✓" : "✕";

    public string TimestampText => Timestamp.ToString("HH:mm:ss");
}
