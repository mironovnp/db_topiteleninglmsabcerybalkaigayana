using System;
using System.Collections.Generic;

namespace CaseChampGui.Services;

/// <summary>Определяет, нужно ли отправить ввод в Text2SQL (как в dbcli).</summary>
public static class SqlInputClassifier
{
    private static readonly HashSet<string> SqlStarts = new(StringComparer.OrdinalIgnoreCase)
    {
        "SELECT", "CREATE", "DROP", "INSERT", "UPDATE", "DELETE",
        "USE", "ALTER", "GRANT", "REVOKE", "SET", "SHOW", "LOAD",
        "BEGIN", "COMMIT", "ROLLBACK", "REGISTER", "LOGIN", "LOGOUT", "CHANGE",
    };

    public static bool TryParseText2SqlPrefix(string input, out string request)
    {
        request = input.Trim();
        if (request.StartsWith("\\ai ", StringComparison.OrdinalIgnoreCase))
        {
            request = request[4..].Trim();
            return true;
        }

        if (request.StartsWith("\\text2sql ", StringComparison.OrdinalIgnoreCase))
        {
            request = request[10..].Trim();
            return true;
        }

        return false;
    }

    public static string GetFirstWord(string input)
    {
        var t = input.TrimStart();
        var i = 0;
        while (i < t.Length && !char.IsWhiteSpace(t[i])) i++;
        return i == 0 ? string.Empty : t[..i].ToUpperInvariant();
    }

    public static bool LooksLikeSql(string input)
    {
        var word = GetFirstWord(input);
        return word.Length > 0 && SqlStarts.Contains(word);
    }

    /// <summary>Явный префикс <c>\ai</c> / <c>\text2sql</c> или текст без SQL-ключевого слова в начале.</summary>
    public static bool ShouldUseText2Sql(string input, out string request)
    {
        if (TryParseText2SqlPrefix(input, out request))
            return !string.IsNullOrWhiteSpace(request);

        request = input.Trim();
        if (string.IsNullOrWhiteSpace(request))
            return false;

        return !LooksLikeSql(request);
    }
}
