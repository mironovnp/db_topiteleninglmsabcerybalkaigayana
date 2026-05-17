using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Text;
using CaseChampGui.Models;

namespace CaseChampGui.Services;

public static class ColumnFilterSqlBuilder
{
    private static readonly HashSet<string> UnsafeTokens = new(StringComparer.OrdinalIgnoreCase)
    {
        ";", "CREATE", "DROP", "INSERT", "UPDATE", "DELETE", "ALTER", "GRANT", "REVOKE",
        "USE ", "BEGIN", "COMMIT", "ROLLBACK", "LOGIN", "REGISTER",
    };

    public static string BuildSelect(
        string table,
        string? orderColumn,
        IEnumerable<ColumnFilter> filters,
        int limit,
        int offset)
    {
        var tableSql = SanitizeIdentifier(table);
        var sb = new StringBuilder();
        sb.Append("SELECT * FROM ").Append(tableSql);

        var clauses = filters.Select(BuildWhereClause).Where(c => !string.IsNullOrEmpty(c)).ToList();
        if (clauses.Count > 0)
        {
            sb.Append(" WHERE ");
            sb.Append(string.Join(" AND ", clauses.Select(c => $"({c})")));
        }

        if (!string.IsNullOrEmpty(orderColumn))
        {
            sb.Append(" ORDER BY ").Append(SanitizeIdentifier(orderColumn));
        }

        sb.Append(CultureInfo.InvariantCulture, $" LIMIT {limit} OFFSET {offset};");
        return sb.ToString();
    }

    public static string? BuildCompareClause(string columnName, string columnType, string op, string rawValue)
    {
        if (string.IsNullOrWhiteSpace(rawValue)) return null;

        var col = SanitizeIdentifier(columnName);
        var lit = FormatLiteral(columnType, rawValue.Trim());
        return op switch
        {
            "=" => $"{col} = {lit}",
            ">" => $"{col} > {lit}",
            "<" => $"{col} < {lit}",
            _ => null,
        };
    }

    public static string NormalizeWhereFragment(string fragment)
    {
        var s = fragment.Trim();
        if (s.StartsWith('(') && s.EndsWith(')') && IsBalancedOuterParens(s))
            s = s[1..^1].Trim();
        return s;
    }

    private static bool IsBalancedOuterParens(string s)
    {
        if (s.Length < 2) return false;
        var depth = 0;
        for (var i = 0; i < s.Length; i++)
        {
            if (s[i] == '(') depth++;
            else if (s[i] == ')')
            {
                depth--;
                if (depth == 0 && i < s.Length - 1) return false;
            }
        }
        return depth == 0;
    }

    public static bool IsSafeWhereFragment(string fragment)
    {
        if (string.IsNullOrWhiteSpace(fragment)) return false;
        if (fragment.Contains(';', StringComparison.Ordinal)) return false;

        var upper = fragment.ToUpperInvariant();
        foreach (var token in UnsafeTokens)
        {
            if (upper.Contains(token, StringComparison.Ordinal))
                return false;
        }

        if (upper.Contains("INTERVAL", StringComparison.Ordinal)
            || upper.Contains("DATE_SUB", StringComparison.Ordinal)
            || upper.Contains("DATEDIFF", StringComparison.Ordinal)
            || upper.Contains("DATE(", StringComparison.Ordinal)
            || upper.Contains("YEAR(", StringComparison.Ordinal)
            || upper.Contains("MONTH(", StringComparison.Ordinal))
        {
            return false;
        }

        return true;
    }

    private static string? BuildWhereClause(ColumnFilter filter) => filter.Kind switch
    {
        ColumnFilterKind.Compare => BuildCompareClause(
            filter.ColumnName,
            filter.ColumnType ?? "TEXT",
            filter.CompareOperator ?? "=",
            filter.CompareValue ?? string.Empty),
        ColumnFilterKind.Text2Sql =>
            IsSafeWhereFragment(filter.WhereFragment ?? string.Empty)
                ? NormalizeWhereFragment(filter.WhereFragment!)
                : null,
        _ => null,
    };

    private static string FormatLiteral(string columnType, string value)
    {
        var t = columnType.Trim().ToUpperInvariant();
        if (t is "INT" or "INTEGER" or "FLOAT" or "DOUBLE" or "REAL")
        {
            if (double.TryParse(value, NumberStyles.Float, CultureInfo.InvariantCulture, out var n)
                || double.TryParse(value, NumberStyles.Float, CultureInfo.CurrentCulture, out n))
            {
                return n.ToString(CultureInfo.InvariantCulture);
            }
        }

        if (t is "BOOL" or "BOOLEAN")
        {
            var lower = value.ToLowerInvariant();
            if (lower is "1" or "true" or "да" or "yes") return "true";
            if (lower is "0" or "false" or "нет" or "no") return "false";
        }

        return "'" + value.Replace("'", "''", StringComparison.Ordinal) + "'";
    }

    private static string SanitizeIdentifier(string name)
        => name.Replace("`", string.Empty, StringComparison.Ordinal).Replace(";", string.Empty, StringComparison.Ordinal).Trim();
}
