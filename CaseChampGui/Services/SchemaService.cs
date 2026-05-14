using System.Collections.Generic;
using System.Threading;
using System.Threading.Tasks;
using CaseChampGui.Models;

namespace CaseChampGui.Services;

public sealed class SchemaService
{
    private readonly IDatabaseClient _client;

    public SchemaService(IDatabaseClient client)
    {
        _client = client;
    }

    public async Task<List<string>> GetDatabasesAsync(CancellationToken token = default)
    {
        var result = await _client.ExecuteAsync("SHOW DATABASES;", false, token);
        var names = new List<string>();
        if (!result.Success) return names;
        foreach (var row in result.Rows)
        {
            if (row.Count > 0 && !string.IsNullOrWhiteSpace(row[0]))
                names.Add(row[0]);
        }
        return names;
    }

    public async Task<bool> UseDatabaseAsync(string database, CancellationToken token = default)
    {
        var safe = database.Replace("`", string.Empty).Replace(";", string.Empty);
        var result = await _client.ExecuteAsync($"USE {safe};", false, token);
        return result.Success;
    }

    public async Task<List<SchemaTable>> GetTablesAsync(bool includeSystemTables = false, CancellationToken token = default)
    {
        var tables = new List<SchemaTable>();
        var tablesResult = await _client.ExecuteAsync("SHOW TABLES;", false, token);
        if (!tablesResult.Success) return tables;

        foreach (var row in tablesResult.Rows)
        {
            if (row.Count == 0) continue;
            var tableName = row[0];
            if (string.IsNullOrWhiteSpace(tableName)) continue;
            if (!includeSystemTables && tableName.StartsWith("sys_", System.StringComparison.OrdinalIgnoreCase))
                continue;

            var columns = await GetColumnsAsync(tableName, token);
            tables.Add(new SchemaTable(tableName, columns));
        }
        return tables;
    }

    public async Task<List<SchemaColumn>> GetColumnsAsync(string tableName, CancellationToken token = default)
    {
        var safe = tableName.Replace("`", string.Empty).Replace(";", string.Empty);
        var result = await _client.ExecuteAsync($"SHOW COLUMNS FROM {safe};", false, token);
        var cols = new List<SchemaColumn>();
        if (!result.Success) return cols;
        foreach (var row in result.Rows)
        {
            if (row.Count < 4) continue;
            var nullable = !string.Equals(row[2], "NO", System.StringComparison.OrdinalIgnoreCase);
            cols.Add(new SchemaColumn(row[0], row[1], row[3], nullable));
        }
        return cols;
    }
}
