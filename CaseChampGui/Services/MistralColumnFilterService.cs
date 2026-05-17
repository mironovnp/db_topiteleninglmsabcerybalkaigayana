using System;
using System.Net.Http;
using System.Net.Http.Headers;
using System.Net.Http.Json;
using System.Text;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;

namespace CaseChampGui.Services;

public sealed class MistralColumnFilterService : IColumnFilterTranslator, IDisposable
{
    private readonly IMistralApiKeyStore _apiKeys;
    private readonly SchemaService _schema;
    private readonly HttpClient _http;

    public MistralColumnFilterService(IMistralApiKeyStore apiKeys, SchemaService schema)
    {
        _apiKeys = apiKeys;
        _schema = schema;
        _http = new HttpClient
        {
            BaseAddress = new Uri("https://api.mistral.ai/"),
            Timeout = TimeSpan.FromSeconds(60),
        };
    }

    public bool IsEnabled => _apiKeys.HasValidKey;

    public async Task<ColumnFilterTranslateResult> TranslateAsync(
        string tableName,
        string columnName,
        string columnType,
        string russianText,
        CancellationToken cancellationToken = default)
    {
        if (string.IsNullOrWhiteSpace(russianText))
            return new ColumnFilterTranslateResult(false, null, "Введите условие фильтра.");

        var apiKey = _apiKeys.GetApiKey();
        if (string.IsNullOrEmpty(apiKey))
        {
            return new ColumnFilterTranslateResult(
                false,
                null,
                "API-ключ Mistral не задан. Укажите ключ в настройках.");
        }

        var tables = await _schema.GetTablesAsync(token: cancellationToken).ConfigureAwait(false);
        var table = tables.Find(t => string.Equals(t.Name, tableName, StringComparison.OrdinalIgnoreCase));
        if (table is null)
            return new ColumnFilterTranslateResult(false, null, $"Таблица «{tableName}» не найдена в схеме.");

        var model = Environment.GetEnvironmentVariable("MISTRAL_MODEL");
        if (string.IsNullOrWhiteSpace(model))
            model = "mistral-small-latest";

        var schemaLine = new StringBuilder();
        schemaLine.Append("TABLE ").Append(table.Name).Append(" (");
        for (var i = 0; i < table.Columns.Count; i++)
        {
            var col = table.Columns[i];
            if (i > 0) schemaLine.Append(", ");
            schemaLine.Append(col.Name).Append(' ').Append(col.Type);
        }
        schemaLine.Append(')');

        var body = new
        {
            model,
            temperature = 0.0,
            messages = new object[]
            {
                new
                {
                    role = "system",
                    content =
                        "Ты переводишь короткие русскоязычные условия фильтрации в SQL-выражение для WHERE учебной СУБД CaseChamp. " +
                        "Верни только JSON: {\"success\":true,\"condition\":\"...\"} или {\"success\":false,\"message\":\"...\"}. " +
                        "Поле condition — только булево выражение (без SELECT, FROM, WHERE, точки с запятой). " +
                        "Разрешено: сравнения = <> < > <= >=, AND OR NOT, LIKE, BETWEEN, IS NULL, литералы в кавычках, CURRENT_DATE или CURRENT_DATE(). " +
                        "Запрещено: DATE(), YEAR(), INTERVAL, DATE_SUB, DATEDIFF, скобки с именами функций, подзапросы. " +
                        "Для «старше N лет» по дате рождения: column < 'ГГГГ-ММ-ДД' (дата = сегодня минус N лет, формат YYYY-MM-DD). " +
                        "Для «младше N лет»: column > 'ГГГГ-ММ-ДД'. " +
                        "Колонка фильтра: \"" + columnName + "\" (тип " + columnType + "). Используй только имена из схемы.",
                },
                new
                {
                    role = "user",
                    content =
                        "Схема:\n" + schemaLine + "\n\n" +
                        "Условие на русском (без указания БД и таблицы):\n" + russianText.Trim(),
                },
            },
        };

        using var request = new HttpRequestMessage(HttpMethod.Post, "v1/chat/completions")
        {
            Content = JsonContent.Create(body),
        };
        request.Headers.Authorization = new AuthenticationHeaderValue("Bearer", apiKey);

        HttpResponseMessage response;
        try
        {
            response = await _http.SendAsync(request, cancellationToken).ConfigureAwait(false);
        }
        catch (Exception ex)
        {
            return new ColumnFilterTranslateResult(false, null, $"Mistral API: {ex.Message}");
        }

        var raw = await response.Content.ReadAsStringAsync(cancellationToken).ConfigureAwait(false);
        if (!response.IsSuccessStatusCode)
        {
            return new ColumnFilterTranslateResult(
                false,
                null,
                $"Mistral HTTP {(int)response.StatusCode}");
        }

        try
        {
            using var doc = JsonDocument.Parse(raw);
            var content = doc.RootElement.GetProperty("choices")[0].GetProperty("message").GetProperty("content")
                .GetString() ?? string.Empty;
            var json = ExtractJsonObject(content);
            if (string.IsNullOrEmpty(json))
                return new ColumnFilterTranslateResult(false, null, "Mistral вернул ответ без JSON.");

            using var parsed = JsonDocument.Parse(json);
            var root = parsed.RootElement;
            if (!root.TryGetProperty("success", out var ok) || !ok.GetBoolean())
            {
                var msg = root.TryGetProperty("message", out var m) ? m.GetString() : null;
                return new ColumnFilterTranslateResult(false, null, msg ?? "Не удалось построить условие.");
            }

            var condition = root.TryGetProperty("condition", out var c) ? c.GetString()?.Trim() : null;
            if (string.IsNullOrWhiteSpace(condition))
                return new ColumnFilterTranslateResult(false, null, "Пустое условие в ответе Mistral.");

            if (!ColumnFilterSqlBuilder.IsSafeWhereFragment(condition))
            {
                return new ColumnFilterTranslateResult(
                    false,
                    null,
                    "Условие отклонено: недопустимые команды или символы.");
            }

            return new ColumnFilterTranslateResult(true, condition, string.Empty);
        }
        catch (Exception ex)
        {
            return new ColumnFilterTranslateResult(false, null, $"Разбор ответа Mistral: {ex.Message}");
        }
    }

    private static string ExtractJsonObject(string text)
    {
        var begin = text.IndexOf('{');
        var end = text.LastIndexOf('}');
        if (begin < 0 || end < 0 || begin > end) return string.Empty;
        return text[begin..(end + 1)];
    }

    public void Dispose() => _http.Dispose();
}
