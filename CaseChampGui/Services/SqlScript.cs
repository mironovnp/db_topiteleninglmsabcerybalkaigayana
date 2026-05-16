using System;
using System.Collections.Generic;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using CaseChampGui.Models;

namespace CaseChampGui.Services;

/// <summary>Splits and runs multi-statement SQL scripts (commands separated by ';').</summary>
public static class SqlScript
{
    public sealed class RunResult
    {
        public bool Success { get; init; }
        public QueryResult LastResult { get; init; } = QueryResult.Fail("Нет результата.");
        public int ExecutedCount { get; init; }
        public int TotalCount { get; init; }
        public string? FailedStatement { get; init; }
        public int? FailedStep { get; init; }
    }

    /// <summary>
    /// Splits SQL into individual statements. Semicolons inside '...' or "..." are ignored.
    /// </summary>
    public static IReadOnlyList<string> SplitStatements(string sql)
    {
        var statements = new List<string>();
        if (string.IsNullOrWhiteSpace(sql))
            return statements;

        var current = new StringBuilder();
        var inSingle = false;
        var inDouble = false;

        for (var i = 0; i < sql.Length; i++)
        {
            var c = sql[i];

            if (c == '\'' && !inDouble)
            {
                if (inSingle && i + 1 < sql.Length && sql[i + 1] == '\'')
                {
                    current.Append("''");
                    i++;
                    continue;
                }

                inSingle = !inSingle;
                current.Append(c);
                continue;
            }

            if (c == '"' && !inSingle)
            {
                inDouble = !inDouble;
                current.Append(c);
                continue;
            }

            if (c == ';' && !inSingle && !inDouble)
            {
                TryAddStatement(current, statements);
                current.Clear();
                continue;
            }

            current.Append(c);
        }

        TryAddStatement(current, statements);
        return statements;
    }

    public static async Task<RunResult> ExecuteAllAsync(
        IDatabaseClient client,
        string sql,
        bool dryRun = false,
        Action<int, int, string>? onStepStarted = null,
        CancellationToken cancellationToken = default)
    {
        var statements = SplitStatements(sql);
        if (statements.Count == 0)
        {
            return new RunResult
            {
                Success = false,
                LastResult = QueryResult.Fail("Пустой SQL."),
                TotalCount = 0,
            };
        }

        if (statements.Count == 1)
        {
            var single = await client.ExecuteAsync(statements[0], dryRun, cancellationToken).ConfigureAwait(false);
            return new RunResult
            {
                Success = single.Success,
                LastResult = single,
                ExecutedCount = single.Success ? 1 : 0,
                TotalCount = 1,
                FailedStatement = single.Success ? null : statements[0],
                FailedStep = single.Success ? null : 1,
            };
        }

        QueryResult last = QueryResult.Fail("Нет результата.");
        var executed = 0;

        for (var i = 0; i < statements.Count; i++)
        {
            cancellationToken.ThrowIfCancellationRequested();
            var statement = statements[i];
            onStepStarted?.Invoke(i + 1, statements.Count, statement);

            last = await client.ExecuteAsync(statement, dryRun, cancellationToken).ConfigureAwait(false);
            if (!last.Success)
            {
                var stepMsg = string.IsNullOrWhiteSpace(last.Message)
                    ? "Запрос завершился ошибкой."
                    : last.Message;
                return new RunResult
                {
                    Success = false,
                    LastResult = new QueryResult
                    {
                        Success = false,
                        Message = $"Шаг {i + 1}/{statements.Count}: {stepMsg}",
                    },
                    ExecutedCount = executed,
                    TotalCount = statements.Count,
                    FailedStatement = statement,
                    FailedStep = i + 1,
                };
            }

            executed++;
        }

        return new RunResult
        {
            Success = true,
            LastResult = last,
            ExecutedCount = executed,
            TotalCount = statements.Count,
        };
    }

    private static void TryAddStatement(StringBuilder buffer, ICollection<string> statements)
    {
        var text = buffer.ToString().Trim();
        if (text.Length == 0)
            return;

        if (!text.EndsWith(';'))
            text += ';';

        statements.Add(text);
    }
}
