using System;
using System.Linq;
using System.Net.Http;
using System.Net.Http.Headers;
using System.Net.Http.Json;
using System.Text;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;

namespace CaseChampGui.Services;

public sealed class MistralText2SqlService : IText2SqlService, IDisposable
{
    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        PropertyNameCaseInsensitive = true,
    };

    private readonly IMistralApiKeyStore _apiKeys;
    private readonly SchemaService _schema;
    private readonly HttpClient _http;

    public MistralText2SqlService(IMistralApiKeyStore apiKeys, SchemaService schema)
    {
        _apiKeys = apiKeys;
        _schema = schema;
        _http = new HttpClient
        {
            BaseAddress = new Uri("https://api.mistral.ai/"),
            Timeout = TimeSpan.FromSeconds(75),
        };
    }

    public bool IsEnabled => _apiKeys.HasValidKey;

    public async Task<Text2SqlResult> TranslateAsync(
        string russianText,
        string? activeDatabase = null,
        CancellationToken cancellationToken = default)
    {
        if (string.IsNullOrWhiteSpace(russianText))
            return new Text2SqlResult(false, null, "Введите запрос на русском языке.");

        var apiKey = _apiKeys.GetApiKey();
        if (string.IsNullOrEmpty(apiKey))
        {
            return new Text2SqlResult(
                false,
                null,
                "API-ключ Mistral не задан. Укажите ключ в этом разделе или в настройках.");
        }

        var schema = await BuildSchemaContextAsync(activeDatabase, cancellationToken).ConfigureAwait(false);
        if (schema.Error is not null)
            return new Text2SqlResult(false, null, schema.Error);

        var model = Environment.GetEnvironmentVariable("MISTRAL_MODEL");
        if (string.IsNullOrWhiteSpace(model))
            model = "mistral-small-latest";

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
                        "Ты переводишь точные русскоязычные запросы пользователя в SQL для учебной СУБД CaseChamp. " +
                        "Используй только переданную схему и каталог баз. Не выдумывай таблицы, колонки, значения и условия. " +
                        "Поддерживаются команды каталога: CREATE DATABASE имя;, DROP DATABASE [IF EXISTS] имя;, USE имя;, SHOW DATABASES;. " +
                        "Если указана активная база, запросы к таблицам относятся к ней; не добавляй USE, если пользователь не просит другую базу или операции с каталогом. " +
                        "Запросы вроде «удали базу X» / «создай базу Y» переводи в DROP DATABASE или CREATE DATABASE. " +
                        "НЕ проверяй права пользователя и НЕ отказывай с формулировками «недостаточно прав» — это решает сервер СУБД. " +
                        "Если для корректного SQL не хватает имени таблицы, колонки, условия, периода или значения, " +
                        "верни JSON: {\"success\":false,\"message\":\"Отсутствует информация о ...\"}. " +
                        "Если информации достаточно, верни только JSON: {\"success\":true,\"sql\":\"...\"}. " +
                        "Несколько команд подряд разделяй точкой с запятой (;), например: DROP DATABASE a; CREATE DATABASE b;. " +
                        "SQL без markdown и без пояснений вне JSON.",
                },
                new
                {
                    role = "user",
                    content = BuildUserPrompt(schema.Text!, russianText.Trim(), activeDatabase),
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
            return new Text2SqlResult(false, null, $"Не удалось подключиться к Mistral API: {ex.Message}");
        }

        var raw = await response.Content.ReadAsStringAsync(cancellationToken).ConfigureAwait(false);
        if (!response.IsSuccessStatusCode)
        {
            return new Text2SqlResult(
                false,
                null,
                $"Mistral API вернул HTTP {(int)response.StatusCode}: {Truncate(raw, 400)}");
        }

        try
        {
            using var doc = JsonDocument.Parse(raw);
            var content = doc.RootElement
                .GetProperty("choices")[0]
                .GetProperty("message")
                .GetProperty("content")
                .GetString() ?? string.Empty;

            var jsonFragment = ExtractJsonObject(content);
            if (string.IsNullOrEmpty(jsonFragment))
                return new Text2SqlResult(false, null, "Mistral API вернул ответ без JSON.");

            using var parsed = JsonDocument.Parse(jsonFragment);
            var root = parsed.RootElement;
            if (!root.TryGetProperty("success", out var successEl) || !successEl.GetBoolean())
            {
                var msg = root.TryGetProperty("message", out var msgEl)
                    ? msgEl.GetString()
                    : null;
                return new Text2SqlResult(
                    false,
                    null,
                    string.IsNullOrWhiteSpace(msg) ? "Отсутствует информация о задаче." : msg!);
            }

            var sql = root.TryGetProperty("sql", out var sqlEl) ? sqlEl.GetString()?.Trim() : null;
            if (string.IsNullOrWhiteSpace(sql))
                return new Text2SqlResult(false, null, "Mistral API вернул пустой SQL.");

            return new Text2SqlResult(true, sql, string.Empty);
        }
        catch (Exception ex)
        {
            return new Text2SqlResult(false, null, $"Не удалось разобрать ответ Mistral API: {ex.Message}");
        }
    }

    private static string BuildUserPrompt(string schemaText, string russianText, string? activeDatabase)
    {
        var sb = new StringBuilder();
        if (!string.IsNullOrWhiteSpace(activeDatabase))
        {
            sb.Append("Активная база данных (выбрана пользователем): ").AppendLine(activeDatabase.Trim());
            sb.AppendLine("Все запросы к таблицам без явного имени базы относятся к этой базе.");
            sb.AppendLine();
        }

        sb.AppendLine("Схема базы данных:");
        sb.AppendLine(schemaText);
        sb.Append("Задача на русском:\n").Append(russianText);
        return sb.ToString();
    }

    private async Task<(string? Text, string? Error)> BuildSchemaContextAsync(string? activeDatabase, CancellationToken token)
    {
        var databases = await _schema.GetDatabasesAsync(token).ConfigureAwait(false);
        var tables = await _schema.GetTablesAsync(token: token).ConfigureAwait(false);

        var sb = new StringBuilder();
        if (!string.IsNullOrWhiteSpace(activeDatabase))
        {
            sb.Append("Текущая активная база: ").AppendLine(activeDatabase.Trim());
            sb.AppendLine();
        }

        sb.AppendLine("Каталог баз (SHOW DATABASES):");
        if (databases.Count == 0)
        {
            sb.AppendLine("(пусто)");
        }
        else
        {
            foreach (var db in databases)
            {
                sb.Append("- ").AppendLine(db);
            }
        }

        sb.AppendLine();
        if (tables.Count == 0)
        {
            sb.AppendLine("Таблицы в текущей базе: (нет)");
        }
        else
        {
            sb.AppendLine("Таблицы в текущей базе:");
        }

        foreach (var table in tables)
        {
            sb.Append("TABLE ").Append(table.Name).Append(" (");
            for (var i = 0; i < table.Columns.Count; i++)
            {
                var col = table.Columns[i];
                if (i > 0) sb.Append(", ");
                sb.Append(col.Name).Append(' ').Append(col.Type);
                if (!col.Nullable) sb.Append(" NOT NULL");
                if (string.Equals(col.Key, "PRI", StringComparison.OrdinalIgnoreCase))
                    sb.Append(" PRIMARY KEY");
                else if (string.Equals(col.Key, "UNI", StringComparison.OrdinalIgnoreCase))
                    sb.Append(" UNIQUE");
            }

            sb.Append(")\n");
        }

        if (databases.Count == 0 && tables.Count == 0)
        {
            return (null, "Нет баз и таблиц — сначала создайте базу (CREATE DATABASE) или выберите существующую.");
        }

        return (sb.ToString(), null);
    }

    private static string ExtractJsonObject(string text)
    {
        var begin = text.IndexOf('{');
        var end = text.LastIndexOf('}');
        if (begin < 0 || end < 0 || begin > end) return string.Empty;
        return text.Substring(begin, end - begin + 1);
    }

    private static string Truncate(string value, int max)
    {
        if (string.IsNullOrEmpty(value) || value.Length <= max) return value;
        return value[..max] + "…";
    }

    public void Dispose() => _http.Dispose();
}
