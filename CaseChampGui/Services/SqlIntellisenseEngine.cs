using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using CaseChampGui.Models;

namespace CaseChampGui.Services;

public enum SqlIntellisenseMode
{
    /// <summary>Классический SQL-редактор.</summary>
    SqlEditor,

    /// <summary>Text2SQL / чат: подсказки только когда ввод уже похож на SQL.</summary>
    Text2SqlAware,
}

public sealed record SqlIntellisenseSuggestion(
    /// <summary>Серый хвост сразу после каретки (только недописанная часть).</summary>
    string GhostSuffix,
    double Confidence,
    /// <summary>Сколько символов перед кареткой заменить при Tab (для cre → CREATE).</summary>
    int ReplaceCharsBeforeCaret = 0,
    /// <summary>Текст при Tab; если null — используется GhostSuffix.</summary>
    string? AcceptText = null);

/// <summary>Вычисляет ghost-подсказки для SQL по схеме и контексту ввода.</summary>
public static class SqlIntellisenseEngine
{
    private const double MinConfidence = 0.9;

    private static readonly string[] CommandKeywords =
    [
        "SELECT", "CREATE", "DROP", "INSERT", "UPDATE", "DELETE",
        "USE", "ALTER", "GRANT", "REVOKE", "SET", "SHOW", "LOAD",
        "BEGIN", "COMMIT", "ROLLBACK", "FROM", "WHERE", "INTO",
        "VALUES", "TABLE", "DATABASE", "INDEX", "COLUMN", "ADD",
        "ON", "PRIMARY", "KEY", "ORDER", "BY", "GROUP", "HAVING",
        "AND", "OR", "NOT", "NULL", "DEFAULT", "APPEND", "CSV",
        "WITH", "AS", "IN", "LIKE", "JOIN", "INNER", "LEFT", "RIGHT",
    ];

    public static SqlIntellisenseSuggestion? Compute(
        string text,
        int caretIndex,
        int selectionStart,
        int selectionLength,
        IReadOnlyList<SchemaTable> tables,
        SqlIntellisenseMode mode)
    {
        try
        {
            if (selectionLength > 0) return null;
            if (string.IsNullOrEmpty(text)) return null;

            caretIndex = Math.Clamp(caretIndex, 0, text.Length);

            if (mode == SqlIntellisenseMode.Text2SqlAware && !SqlInputClassifier.LooksLikeSql(text))
                return null;

            if (!IsCaretAtTokenEnd(text, caretIndex))
                return null;

            var statement = ExtractCurrentStatement(text, caretIndex);
            var (partial, _) = GetPartialToken(text, caretIndex);
            var tokens = Tokenize(statement.Text);
            var upperPartial = partial.ToUpperInvariant();

            var suggestion = TrySuggest(tokens, partial, upperPartial, statement, tables, text, caretIndex);
            if (suggestion is null || suggestion.Confidence < MinConfidence)
                return null;

            if (suggestion.GhostSuffix.Length == 0)
                return null;

            var suffix = ApplySpacingBeforeSuffix(text, caretIndex, partial, suggestion.GhostSuffix);
            return suggestion with { GhostSuffix = suffix };
        }
        catch
        {
            return null;
        }
    }

    /// <summary>Пробел только перед новым словом, не при дописывании текущего.</summary>
    private static string ApplySpacingBeforeSuffix(string text, int caret, string partial, string suffix)
    {
        if (string.IsNullOrEmpty(suffix)) return suffix;
        // Дописываем текущий токен (CREATE D → ATABASE): пробел не нужен.
        if (!string.IsNullOrEmpty(partial)) return suffix;

        if (caret <= 0 || caret > text.Length) return suffix;

        var last = text[caret - 1];
        var first = suffix[0];

        if (char.IsWhiteSpace(last)) return suffix;
        if (first is ' ' or ',' or '(' or ';' or ')' or '.') return suffix;
        if (char.IsLetterOrDigit(last) && (char.IsLetterOrDigit(first) || first is '_' or '`'))
            return " " + suffix;

        return suffix;
    }

    private sealed record StatementSlice(string Text, int OffsetInFull);

    private static StatementSlice ExtractCurrentStatement(string text, int caret)
    {
        caret = Math.Clamp(caret, 0, text.Length);
        var searchFrom = caret > 0 ? caret - 1 : 0;
        var start = text.LastIndexOf(';', searchFrom) + 1;
        if (start < 0) start = 0;
        return new StatementSlice(text[start..caret], start);
    }

    private static bool IsCaretAtTokenEnd(string text, int caret)
    {
        if (caret < text.Length)
        {
            var ch = text[caret];
            if (!char.IsWhiteSpace(ch) && ch != ';' && ch != ',' && ch != '(' && ch != ')')
                return false;
        }
        return true;
    }

    private static (string Partial, int TokenStart) GetPartialToken(string text, int caret)
    {
        caret = Math.Clamp(caret, 0, text.Length);
        var start = caret;
        while (start > 0)
        {
            var prev = text[start - 1];
            if (char.IsWhiteSpace(prev) || prev is ';' or ',' or '(' or ')')
                break;
            start--;
        }

        return (text[start..caret], start);
    }

    private static List<string> Tokenize(string sql)
    {
        var tokens = new List<string>();
        var i = 0;
        while (i < sql.Length)
        {
            if (char.IsWhiteSpace(sql[i]))
            {
                i++;
                continue;
            }

            if (sql[i] is ';' or ',' or '(' or ')')
            {
                tokens.Add(sql[i].ToString());
                i++;
                continue;
            }

            var sb = new StringBuilder();
            while (i < sql.Length && !char.IsWhiteSpace(sql[i]) && sql[i] is not ';' and not ',' and not '(' and not ')')
            {
                sb.Append(sql[i]);
                i++;
            }

            if (sb.Length > 0)
                tokens.Add(sb.ToString());
        }

        return tokens;
    }

    private static string Upper(string s) => s.ToUpperInvariant();

    private static SqlIntellisenseSuggestion? TrySuggest(
        List<string> tokens,
        string partial,
        string upperPartial,
        StatementSlice statement,
        IReadOnlyList<SchemaTable> tables,
        string fullText,
        int caret)
    {
        if (tokens.Count == 0)
            return CompleteKeyword(partial, upperPartial, CommandKeywords, trailingSpace: true);

        // Убираем незавершённый последний токен из анализа контекста.
        var contextTokens = new List<string>(tokens);
        var hasPartial = !string.IsNullOrEmpty(partial) &&
                         contextTokens.Count > 0 &&
                         contextTokens[^1] == partial;
        if (hasPartial)
            contextTokens.RemoveAt(contextTokens.Count - 1);

        var first = contextTokens.Count > 0 ? Upper(contextTokens[0]) : upperPartial;

        if (contextTokens.Count == 0)
            return CompleteKeyword(partial, upperPartial, CommandKeywords, trailingSpace: true);

        if (first is "SELECT")
            return SuggestAfterSelect(contextTokens, partial, upperPartial, tables);

        if (first is "FROM" && contextTokens.Count == 1)
            return CompleteKeyword(partial, upperPartial, CommandKeywords, trailingSpace: true);

        if (first is "INSERT")
            return SuggestAfterInsert(contextTokens, partial, upperPartial, tables);

        if (first is "UPDATE")
            return SuggestAfterUpdate(contextTokens, partial, upperPartial, tables);

        if (first is "DELETE")
            return SuggestAfterDelete(contextTokens, partial, upperPartial, tables);

        if (first is "CREATE")
            return SuggestAfterCreate(contextTokens, partial, upperPartial, tables);

        if (first is "DROP")
            return SuggestAfterDrop(contextTokens, partial, upperPartial, tables);

        if (first is "ALTER")
            return SuggestAfterAlter(contextTokens, partial, upperPartial, tables);

        if (first is "USE")
            return SuggestDatabaseName(contextTokens, partial, tables);

        if (first is "SHOW")
            return SuggestAfterShow(contextTokens, partial, upperPartial);

        if (CommandKeywords.Contains(first) && contextTokens.Count == 1 && string.IsNullOrEmpty(partial))
            return new SqlIntellisenseSuggestion(" ", 0.95);

        if (contextTokens.Count == 1 && hasPartial)
            return CompleteKeyword(partial, upperPartial, CommandKeywords, trailingSpace: true);

        return null;
    }

    private static SqlIntellisenseSuggestion? SuggestAfterSelect(
        List<string> tokens, string partial, string upperPartial, IReadOnlyList<SchemaTable> tables)
    {
        var hasFrom = tokens.Any(t => Upper(t) == "FROM");
        if (!hasFrom)
        {
            if (tokens.Count == 1 && string.IsNullOrEmpty(partial))
                return SuggestSelectStarFrom(tables);

            if (tokens.Count >= 2 && Upper(tokens[1]) != "FROM")
            {
                if (string.IsNullOrEmpty(partial))
                    return new SqlIntellisenseSuggestion(" FROM ", 0.95);
                return CompleteKeyword(partial, upperPartial, ["FROM"], trailingSpace: true);
            }

            if (tokens.Count >= 2 && Upper(tokens[^1]) == "FROM" && string.IsNullOrEmpty(partial))
                return SuggestTableName(partial, tables, trailingSemicolon: true);

            if (tokens.Count >= 2 && Upper(tokens[^2]) == "FROM")
                return SuggestTableName(partial, tables, trailingSemicolon: true);
        }
        else
        {
            var fromIdx = tokens.FindIndex(t => Upper(t) == "FROM");
            if (fromIdx >= 0 && fromIdx + 1 < tokens.Count)
            {
                var tableName = tokens[fromIdx + 1];
                var table = FindTable(tables, tableName);
                if (table is not null && tokens.Count == fromIdx + 2 && string.IsNullOrEmpty(partial))
                    return new SqlIntellisenseSuggestion(";", 0.92);

                if (table is not null && tokens.Count >= 2)
                {
                    var hasWhere = tokens.Any(t => Upper(t) == "WHERE");
                    if (!hasWhere && string.IsNullOrEmpty(partial))
                        return new SqlIntellisenseSuggestion(" WHERE ", 0.9);
                }
            }
        }

        return CompleteKeyword(partial, upperPartial, CommandKeywords, trailingSpace: true);
    }

    private static SqlIntellisenseSuggestion? SuggestSelectStarFrom(IReadOnlyList<SchemaTable> tables)
    {
        if (tables.Count == 0)
            return new SqlIntellisenseSuggestion("* FROM ", 0.92);

        if (tables.Count == 1)
            return new SqlIntellisenseSuggestion($"* FROM {tables[0].Name};", 0.95);

        return new SqlIntellisenseSuggestion("* FROM ", 0.93);
    }

    private static SqlIntellisenseSuggestion? SuggestAfterInsert(
        List<string> tokens, string partial, string upperPartial, IReadOnlyList<SchemaTable> tables)
    {
        if (tokens.Count == 1)
        {
            if (string.IsNullOrEmpty(partial))
                return new SqlIntellisenseSuggestion("INTO ", 0.95);
            return CompleteKeyword(partial, upperPartial, ["INTO"], trailingSpace: true);
        }

        if (tokens.Count == 2 && Upper(tokens[1]) == "INTO" && string.IsNullOrEmpty(partial))
            return SuggestTableName(partial, tables, trailingSemicolon: false);

        if (tokens.Count >= 3 && Upper(tokens[1]) == "INTO")
        {
            var tableName = tokens[2];
            var table = FindTable(tables, tableName);
            if (table is null)
                return SuggestTableName(partial, tables, trailingSemicolon: false);

            var hasValues = tokens.Any(t => Upper(t) == "VALUES");
            var hasOpen = tokens.Any(t => t == "(");

            if (!hasOpen && string.IsNullOrEmpty(partial))
                return SuggestInsertColumnList(table, firstOnly: true);

            if (hasOpen && !hasValues)
            {
                var openIdx = tokens.FindIndex(t => t == "(");
                var colsInside = tokens.Skip(openIdx + 1).TakeWhile(t => t != ")").ToList();
                if (colsInside.Count == 0 && string.IsNullOrEmpty(partial))
                    return SuggestFirstInsertColumn(table);

                if (colsInside.Count >= 1 && !tokens.Any(t => t == ")") && string.IsNullOrEmpty(partial))
                    return SuggestRemainingInsertColumns(table, colsInside);
            }
        }

        if (tokens.Count >= 2 && Upper(tokens[1]) == "INTO" && !string.IsNullOrEmpty(partial))
            return SuggestTableName(partial, tables, trailingSemicolon: false);

        return CompleteKeyword(partial, upperPartial, CommandKeywords, trailingSpace: true);
    }

    private static SqlIntellisenseSuggestion? SuggestFirstInsertColumn(SchemaTable table)
    {
        if (table.Columns.Count == 0)
            return new SqlIntellisenseSuggestion(")", 0.9);

        return new SqlIntellisenseSuggestion(table.Columns[0].Name, 0.95);
    }

    private static SqlIntellisenseSuggestion? SuggestRemainingInsertColumns(SchemaTable table, List<string> already)
    {
        var used = new HashSet<string>(already, StringComparer.OrdinalIgnoreCase);
        var remaining = table.Columns.Where(c => !used.Contains(c.Name)).Select(c => c.Name).ToList();
        if (remaining.Count == 0)
            return new SqlIntellisenseSuggestion(") VALUES (", 0.95);

        var joined = string.Join(", ", remaining);
        return new SqlIntellisenseSuggestion($", {joined}) VALUES (", 0.93);
    }

    private static SqlIntellisenseSuggestion? SuggestInsertColumnList(SchemaTable table, bool firstOnly)
    {
        if (table.Columns.Count == 0)
            return new SqlIntellisenseSuggestion("()", 0.9);

        if (firstOnly)
            return new SqlIntellisenseSuggestion($"({table.Columns[0].Name}", 0.95);

        var cols = string.Join(", ", table.Columns.Select(c => c.Name));
        return new SqlIntellisenseSuggestion($"({cols}) VALUES (", 0.93);
    }

    private static SqlIntellisenseSuggestion? SuggestAfterUpdate(
        List<string> tokens, string partial, string upperPartial, IReadOnlyList<SchemaTable> tables)
    {
        if (tokens.Count == 1 && string.IsNullOrEmpty(partial))
            return SuggestTableName(partial, tables, trailingSemicolon: false);

        if (tokens.Count == 2 && string.IsNullOrEmpty(partial))
            return new SqlIntellisenseSuggestion(" SET ", 0.95);

        if (tokens.Count >= 3 && Upper(tokens[2]) == "SET")
        {
            var table = FindTable(tables, tokens[1]);
            if (table is not null)
            {
                if (tokens.Count == 3 && string.IsNullOrEmpty(partial))
                    return SuggestColumnName(partial, table, trailingSpace: true);

                if (tokens.Count >= 4 && string.IsNullOrEmpty(partial) && !tokens.Any(t => Upper(t) == "WHERE"))
                    return new SqlIntellisenseSuggestion(" =  WHERE ", 0.9);
            }
        }

        if (tokens.Count == 2 && !string.IsNullOrEmpty(partial))
            return SuggestTableName(partial, tables, trailingSemicolon: false);

        return CompleteKeyword(partial, upperPartial, CommandKeywords, trailingSpace: true);
    }

    private static SqlIntellisenseSuggestion? SuggestAfterDelete(
        List<string> tokens, string partial, string upperPartial, IReadOnlyList<SchemaTable> tables)
    {
        if (tokens.Count == 1)
        {
            if (string.IsNullOrEmpty(partial))
                return new SqlIntellisenseSuggestion("FROM ", 0.95);
            return CompleteKeyword(partial, upperPartial, ["FROM"], trailingSpace: true);
        }

        if (tokens.Count < 2)
            return CompleteKeyword(partial, upperPartial, CommandKeywords, trailingSpace: true);

        if (tokens.Count >= 3 && Upper(tokens[1]) == "FROM" && string.IsNullOrEmpty(partial) &&
            !tokens.Any(t => Upper(t) == "WHERE"))
            return new SqlIntellisenseSuggestion(" WHERE ", 0.92);

        if (Upper(tokens[1]) == "FROM")
            return SuggestTableName(partial, tables, trailingSemicolon: false);

        return CompleteKeyword(partial, upperPartial, CommandKeywords, trailingSpace: true);
    }

    private static SqlIntellisenseSuggestion? SuggestAfterCreate(
        List<string> tokens, string partial, string upperPartial, IReadOnlyList<SchemaTable> tables)
    {
        if (tokens.Count == 1)
            return CompleteKeyword(partial, upperPartial, ["TABLE", "DATABASE", "INDEX"], trailingSpace: true);

        if (tokens.Count < 2)
            return CompleteKeyword(partial, upperPartial, CommandKeywords, trailingSpace: true);

        var kind = Upper(tokens[1]);

        if (kind == "DATABASE")
        {
            if (tokens.Count == 2 && string.IsNullOrEmpty(partial))
                return new SqlIntellisenseSuggestion("name;", 0.9);
        }

        if (kind == "TABLE")
        {
            if (tokens.Count == 2 && string.IsNullOrEmpty(partial))
                return new SqlIntellisenseSuggestion("name ", 0.9);

            if (tokens.Count == 3 && string.IsNullOrEmpty(partial))
                return new SqlIntellisenseSuggestion("(id INT PRIMARY KEY", 0.95);

            if (tokens.Count >= 4)
            {
                var hasOpen = tokens.Any(t => t == "(");
                if (hasOpen)
                {
                    var insideText = ExtractInsideParensText(tokens);
                    if (string.IsNullOrWhiteSpace(insideText) && string.IsNullOrEmpty(partial))
                        return new SqlIntellisenseSuggestion("id INT PRIMARY KEY", 0.95);

                    if (!string.IsNullOrWhiteSpace(insideText) &&
                        LooksLikeCompleteColumnDef(insideText) &&
                        string.IsNullOrEmpty(partial))
                        return new SqlIntellisenseSuggestion(", name TEXT);", 0.93);
                }
            }

            if (tokens.Count == 3 && !string.IsNullOrEmpty(partial))
                return CompleteIdentifier(partial, [], trailingSuffix: " (id INT PRIMARY KEY", confidence: 0.9);
        }

        if (kind == "INDEX" && tokens.Count == 2 && string.IsNullOrEmpty(partial))
            return new SqlIntellisenseSuggestion("idx_name ON table_name(column);", 0.9);

        return CompleteKeyword(partial, upperPartial, CommandKeywords, trailingSpace: true);
    }

    private static SqlIntellisenseSuggestion? SuggestAfterDrop(
        List<string> tokens, string partial, string upperPartial, IReadOnlyList<SchemaTable> tables)
    {
        if (tokens.Count == 1)
            return CompleteKeyword(partial, upperPartial, ["TABLE", "DATABASE", "INDEX"], trailingSpace: true);

        if (tokens.Count < 2)
            return CompleteKeyword(partial, upperPartial, CommandKeywords, trailingSpace: true);

        var kind = Upper(tokens[1]);

        if (kind == "TABLE")
            return SuggestTableName(partial, tables, trailingSemicolon: true);

        if (kind == "DATABASE" && string.IsNullOrEmpty(partial))
            return new SqlIntellisenseSuggestion("name;", 0.9);

        return CompleteKeyword(partial, upperPartial, CommandKeywords, trailingSpace: true);
    }

    private static SqlIntellisenseSuggestion? SuggestAfterAlter(
        List<string> tokens, string partial, string upperPartial, IReadOnlyList<SchemaTable> tables)
    {
        if (tokens.Count == 1)
        {
            if (string.IsNullOrEmpty(partial))
                return new SqlIntellisenseSuggestion("TABLE ", 0.95);
            return CompleteKeyword(partial, upperPartial, ["TABLE"], trailingSpace: true);
        }

        if (tokens.Count < 2)
            return CompleteKeyword(partial, upperPartial, CommandKeywords, trailingSpace: true);

        if (Upper(tokens[1]) == "TABLE")
            return SuggestTableName(partial, tables, trailingSemicolon: false);

        if (tokens.Count == 3 && string.IsNullOrEmpty(partial))
            return new SqlIntellisenseSuggestion(" ADD COLUMN name TYPE;", 0.92);

        return CompleteKeyword(partial, upperPartial, CommandKeywords, trailingSpace: true);
    }

    private static SqlIntellisenseSuggestion? SuggestAfterShow(
        List<string> tokens, string partial, string upperPartial)
    {
        if (tokens.Count == 1 && string.IsNullOrEmpty(partial))
            return CompleteKeyword(partial, upperPartial,
                ["TABLES", "DATABASES", "COLUMNS"],
                trailingSpace: true);

        if (tokens.Count == 2 && Upper(tokens[1]) == "COLUMNS" && string.IsNullOrEmpty(partial))
            return new SqlIntellisenseSuggestion("FROM table_name;", 0.92);

        return CompleteKeyword(partial, upperPartial, CommandKeywords, trailingSpace: true);
    }

    private static SqlIntellisenseSuggestion? SuggestDatabaseName(
        List<string> tokens, string partial, IReadOnlyList<SchemaTable> tables)
    {
        if (tokens.Count == 1 && string.IsNullOrEmpty(partial))
            return new SqlIntellisenseSuggestion("database_name;", 0.9);

        return CompleteIdentifier(partial, [], trailingSuffix: ";", confidence: 0.9);
    }

    private static SqlIntellisenseSuggestion? SuggestTableName(
        string partial,
        IReadOnlyList<SchemaTable> tables,
        bool trailingSemicolon)
    {
        var names = tables.Select(t => t.Name).ToList();
        if (names.Count == 0)
            return null;

        var suffix = CompleteUniqueSuffix(partial, names);
        if (suffix is null) return null;

        if (trailingSemicolon && names.Count == 1 &&
            names[0].StartsWith(partial, StringComparison.OrdinalIgnoreCase))
            return new SqlIntellisenseSuggestion(suffix + ";", 0.95);

        return new SqlIntellisenseSuggestion(suffix, 0.95);
    }

    private static SqlIntellisenseSuggestion? SuggestColumnName(
        string partial,
        SchemaTable table,
        bool trailingSpace)
    {
        var names = table.Columns.Select(c => c.Name).ToList();
        var suffix = CompleteUniqueSuffix(partial, names);
        if (suffix is null) return null;
        return new SqlIntellisenseSuggestion(suffix + (trailingSpace ? " " : ""), 0.95);
    }

    private static SqlIntellisenseSuggestion? CompleteKeyword(
        string partial,
        string upperPartial,
        IEnumerable<string> candidates,
        bool trailingSpace)
    {
        var match = FindUniqueKeywordMatch(upperPartial, candidates);
        if (match is null) return null;

        var rest = match[upperPartial.Length..];
        if (rest.Length == 0) return null;

        var tail = trailingSpace ? rest + " " : rest;

        if (ShouldNormalizeKeywordToUppercase(partial, match))
        {
            var full = trailingSpace ? match + " " : match;
            return new SqlIntellisenseSuggestion(tail, 0.95, partial.Length, AcceptText: full);
        }

        return new SqlIntellisenseSuggestion(tail, 0.95);
    }

    private static string? FindUniqueKeywordMatch(string upperPartial, IEnumerable<string> candidates)
    {
        var cmp = StringComparison.OrdinalIgnoreCase;
        var matches = candidates
            .Where(c => c.StartsWith(upperPartial, cmp))
            .Distinct(StringComparer.OrdinalIgnoreCase)
            .ToList();

        if (matches.Count == 1)
            return matches[0];

        if (matches.Count > 1)
        {
            var common = CommonContinuation(upperPartial, matches);
            if (common.Length > upperPartial.Length)
                return common;
        }

        return null;
    }

    private static bool ShouldNormalizeKeywordToUppercase(string partial, string match) =>
        !string.IsNullOrEmpty(partial) &&
        partial.Length < match.Length &&
        partial.Any(char.IsLower);

    private static SqlIntellisenseSuggestion? CompleteIdentifier(
        string partial,
        IEnumerable<string> candidates,
        string trailingSuffix,
        double confidence)
    {
        var suffix = CompleteUniqueSuffix(partial, candidates);
        if (suffix is null && string.IsNullOrEmpty(partial))
            return new SqlIntellisenseSuggestion(trailingSuffix.TrimStart(), confidence);

        if (suffix is null) return null;
        return new SqlIntellisenseSuggestion(suffix + trailingSuffix, confidence);
    }

    private static string? CompleteUniqueSuffix(string partial, IEnumerable<string> candidates)
    {
        var cmp = StringComparison.OrdinalIgnoreCase;
        var matches = candidates
            .Where(c => c.StartsWith(partial, cmp))
            .Distinct(StringComparer.OrdinalIgnoreCase)
            .ToList();

        if (matches.Count == 0) return null;

        if (matches.Count == 1)
        {
            var m = matches[0];
            if (m.Length <= partial.Length) return null;
            return m[partial.Length..];
        }

        var common = CommonContinuation(partial, matches);
        if (common.Length > partial.Length)
            return common[partial.Length..];

        return null;
    }

    private static string CommonContinuation(string partial, IReadOnlyList<string> matches)
    {
        if (matches.Count == 0) return partial;
        var common = matches[0];
        for (var i = 1; i < matches.Count; i++)
        {
            var m = matches[i];
            var max = Math.Min(common.Length, m.Length);
            var j = 0;
            while (j < max && char.ToUpperInvariant(common[j]) == char.ToUpperInvariant(m[j]))
                j++;
            common = common[..j];
            if (common.Length <= partial.Length)
                break;
        }

        return common;
    }

    private static SchemaTable? FindTable(IReadOnlyList<SchemaTable> tables, string name) =>
        tables.FirstOrDefault(t => t.Name.Equals(name, StringComparison.OrdinalIgnoreCase));

    private static string ExtractInsideParensText(List<string> tokens)
    {
        var open = tokens.FindIndex(t => t == "(");
        if (open < 0) return string.Empty;
        var parts = new List<string>();
        for (var i = open + 1; i < tokens.Count; i++)
        {
            if (tokens[i] == ")") break;
            if (tokens[i] != ",")
                parts.Add(tokens[i]);
        }
        return string.Join(" ", parts);
    }

    private static bool LooksLikeCompleteColumnDef(string token) =>
        token.Contains("INT", StringComparison.OrdinalIgnoreCase) ||
        token.Contains("TEXT", StringComparison.OrdinalIgnoreCase) ||
        token.Contains("PRIMARY", StringComparison.OrdinalIgnoreCase);
}
