using System;
using System.Collections.Generic;
using System.IO;
using System.Net.Http;
using System.Net.Http.Headers;
using System.Net.Http.Json;
using System.Text.Json;
using System.Text.Json.Serialization;
using System.Threading;
using System.Threading.Tasks;
using CaseChampGui.Models;

namespace CaseChampGui.Services;

public sealed class HttpDatabaseClient : IDatabaseClient, IDisposable
{
    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull,
    };

    private readonly HttpClient _http;
    private readonly SemaphoreSlim _httpSerial = new(1, 1);
    private readonly object _sync = new();

    private string _host = "127.0.0.1";
    private int _port = 8080;
    private ConnectionStatus _status = ConnectionStatus.Disconnected;
    private string? _currentDb;
    private string? _currentUser;
    private bool _isGlobalAdmin;
    private string? _sessionId;

    public HttpDatabaseClient()
    {
        _http = new HttpClient
        {
            Timeout = TimeSpan.FromSeconds(45),
        };
    }

    public ConnectionStatus Status
    {
        get { lock (_sync) return _status; }
        private set
        {
            lock (_sync) { if (_status == value) return; _status = value; }
            StateChanged?.Invoke(this, EventArgs.Empty);
        }
    }

    public string Host { get { lock (_sync) return _host; } }
    public int Port { get { lock (_sync) return _port; } }
    public string? CurrentDb { get { lock (_sync) return _currentDb; } }
    public string? CurrentUser { get { lock (_sync) return _currentUser; } }
    public bool IsGlobalAdmin { get { lock (_sync) return _isGlobalAdmin; } }
    public string? SessionId { get { lock (_sync) return _sessionId; } }

    /// <summary>Отвечает /ping с import_csv_v2 — сервер умеет читать table из multipart.</summary>
    public bool SupportsModernCsvImport { get; private set; }

    public event EventHandler? StateChanged;

    public void Configure(string host, int port)
    {
        lock (_sync)
        {
            if (_host == host && _port == port) return;
            _host = host;
            _port = port;
            _status = ConnectionStatus.Disconnected;
            _sessionId = null;
            _currentDb = null;
            _currentUser = null;
            _isGlobalAdmin = false;
        }
        StateChanged?.Invoke(this, EventArgs.Empty);
    }

    public async Task<bool> PingAsync(CancellationToken cancellationToken = default)
    {
        await _httpSerial.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            Status = ConnectionStatus.Connecting;
            try
            {
                using var cts = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
                cts.CancelAfter(TimeSpan.FromSeconds(4));
                var url = $"http://{Host}:{Port}/ping";
                using var response = await _http.GetAsync(url, cts.Token).ConfigureAwait(false);
                var ok = response.IsSuccessStatusCode;
                SupportsModernCsvImport = false;
                if (ok)
                {
                    var body = await response.Content.ReadAsStringAsync(cts.Token).ConfigureAwait(false);
                    try
                    {
                        using var doc = JsonDocument.Parse(body);
                        if (doc.RootElement.TryGetProperty("import_csv_v2", out var flag)
                            && flag.ValueKind == JsonValueKind.True)
                        {
                            SupportsModernCsvImport = true;
                        }
                    }
                    catch
                    {
                    }
                }
                Status = ok ? ConnectionStatus.Connected : ConnectionStatus.Failed;
                return ok;
            }
            catch
            {
                Status = ConnectionStatus.Failed;
                return false;
            }
        }
        finally
        {
            _httpSerial.Release();
        }
    }

    public async Task<QueryResult> ExecuteAsync(string sql, bool dryRun = false, CancellationToken cancellationToken = default)
    {
        if (string.IsNullOrWhiteSpace(sql))
        {
            return QueryResult.Fail("Запрос пуст.");
        }

        await _httpSerial.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            var request = new QueryRequest
            {
                Sql = sql,
                DryRun = dryRun,
                CurrentDb = CurrentDb ?? string.Empty,
                CurrentUser = CurrentUser ?? string.Empty,
                SessionId = SessionId,
            };

            var url = $"http://{Host}:{Port}/query";

            try
            {
                using var response = await _http.PostAsJsonAsync(url, request, JsonOptions, cancellationToken).ConfigureAwait(false);
                if (!response.IsSuccessStatusCode)
                {
                    Status = ConnectionStatus.Failed;
                    return QueryResult.Fail($"HTTP {(int)response.StatusCode}: {response.ReasonPhrase}");
                }

                var json = await response.Content.ReadAsStringAsync(cancellationToken).ConfigureAwait(false);
                var result = JsonSerializer.Deserialize<QueryResult>(json, new JsonSerializerOptions
                {
                    PropertyNameCaseInsensitive = true,
                }) ?? QueryResult.Fail("Пустой ответ сервера.");

                UpdateSessionFromResult(result);
                Status = ConnectionStatus.Connected;
                return result;
            }
            catch (TaskCanceledException) when (cancellationToken.IsCancellationRequested)
            {
                return QueryResult.Fail("Запрос отменён.");
            }
            catch (TaskCanceledException)
            {
                Status = ConnectionStatus.Failed;
                return QueryResult.Fail("Превышено время ожидания ответа сервера.");
            }
            catch (HttpRequestException ex)
            {
                Status = ConnectionStatus.Failed;
                return QueryResult.Fail($"Нет связи с сервером: {ex.Message}");
            }
            catch (Exception ex)
            {
                Status = ConnectionStatus.Failed;
                return QueryResult.Fail($"Ошибка клиента: {ex.Message}");
            }
        }
        finally
        {
            _httpSerial.Release();
        }
    }

    private void UpdateSessionFromResult(QueryResult result)
    {
        var changed = false;
        lock (_sync)
        {
            if (!string.IsNullOrEmpty(result.SessionId) && result.SessionId != _sessionId)
            {
                _sessionId = result.SessionId;
                changed = true;
            }
            if (result.CurrentDb is not null)
            {
                var nextDb = string.IsNullOrWhiteSpace(result.CurrentDb) ? null : result.CurrentDb;
                if (nextDb != _currentDb)
                {
                    _currentDb = nextDb;
                    changed = true;
                }
            }
            if (result.CurrentUser is not null && result.CurrentUser != _currentUser)
            {
                _currentUser = result.CurrentUser;
                changed = true;
            }
            if (!string.IsNullOrEmpty(_currentUser) || !string.IsNullOrEmpty(result.CurrentUser))
            {
                if (result.IsAdmin != _isGlobalAdmin)
                {
                    _isGlobalAdmin = result.IsAdmin;
                    changed = true;
                }
            }
        }
        if (changed)
        {
            StateChanged?.Invoke(this, EventArgs.Empty);
        }
    }

    public void ClearSession()
    {
        var changed = false;
        lock (_sync)
        {
            if (_sessionId is not null) { _sessionId = null; changed = true; }
            if (_currentDb is not null) { _currentDb = null; changed = true; }
            if (_currentUser is not null) { _currentUser = null; changed = true; }
            if (_isGlobalAdmin) { _isGlobalAdmin = false; changed = true; }
        }
        if (changed)
        {
            StateChanged?.Invoke(this, EventArgs.Empty);
        }
    }

    public async Task<CsvImportResult> ImportCsvAsync(
        string tableName,
        string fileName,
        Stream fileContent,
        bool append = false,
        CancellationToken cancellationToken = default)
    {
        if (string.IsNullOrWhiteSpace(tableName))
            return new CsvImportResult { Success = false, Message = "Укажите таблицу." };
        if (fileContent is null || !fileContent.CanRead)
            return new CsvImportResult { Success = false, Message = "Файл не выбран." };

        await _httpSerial.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            using var form = new MultipartFormDataContent();
            if (!string.IsNullOrEmpty(SessionId))
                form.Add(new StringContent(SessionId), "session_id");
            if (!string.IsNullOrEmpty(CurrentDb))
                form.Add(new StringContent(CurrentDb), "database");
            form.Add(new StringContent(tableName.Trim()), "table");
            form.Add(new StringContent(append ? "true" : "false"), "append");

            var filePart = new StreamContent(fileContent);
            filePart.Headers.ContentType = new MediaTypeHeaderValue("text/csv");
            form.Add(filePart, "file", string.IsNullOrWhiteSpace(fileName) ? "import.csv" : fileName);

            // Дублируем поля в query: старый dbserver читал только req.params, не multipart.
            var query = new List<string>
            {
                $"table={Uri.EscapeDataString(tableName.Trim())}",
                $"append={Uri.EscapeDataString(append ? "true" : "false")}",
            };
            if (!string.IsNullOrEmpty(SessionId))
                query.Add($"session_id={Uri.EscapeDataString(SessionId)}");
            if (!string.IsNullOrEmpty(CurrentDb))
                query.Add($"database={Uri.EscapeDataString(CurrentDb)}");

            var url = $"http://{Host}:{Port}/import-csv?{string.Join("&", query)}";
            using var response = await _http.PostAsync(url, form, cancellationToken).ConfigureAwait(false);
            var json = await response.Content.ReadAsStringAsync(cancellationToken).ConfigureAwait(false);

            if (!response.IsSuccessStatusCode)
            {
                Status = ConnectionStatus.Failed;
                return new CsvImportResult
                {
                    Success = false,
                    Message = $"HTTP {(int)response.StatusCode}: {json}",
                };
            }

            using var doc = JsonDocument.Parse(json);
            var root = doc.RootElement;
            var result = new CsvImportResult
            {
                Success = root.TryGetProperty("success", out var ok) && ok.GetBoolean(),
                Message = root.TryGetProperty("message", out var msg) ? msg.GetString() ?? string.Empty : string.Empty,
                ServerFileName = root.TryGetProperty("server_filename", out var fn) ? fn.GetString() : null,
                Sql = root.TryGetProperty("sql", out var sql) ? sql.GetString() : null,
                AffectedRows = root.TryGetProperty("affected_rows", out var ar) && ar.TryGetInt32(out var n) ? n : 0,
            };

            var queryLike = JsonSerializer.Deserialize<QueryResult>(json, new JsonSerializerOptions
            {
                PropertyNameCaseInsensitive = true,
            });
            if (queryLike is not null)
                UpdateSessionFromResult(queryLike);

            Status = ConnectionStatus.Connected;
            return result;
        }
        catch (TaskCanceledException) when (cancellationToken.IsCancellationRequested)
        {
            return new CsvImportResult { Success = false, Message = "Импорт отменён." };
        }
        catch (Exception ex)
        {
            Status = ConnectionStatus.Failed;
            return new CsvImportResult { Success = false, Message = ex.Message };
        }
        finally
        {
            _httpSerial.Release();
        }
    }

    public async Task LogoutAsync(CancellationToken cancellationToken = default)
    {
        try
        {
            await ExecuteAsync("LOGOUT;", false, cancellationToken).ConfigureAwait(false);
        }
        catch
        {
        }
        ClearSession();
    }

    public void Dispose()
    {
        _httpSerial.Dispose();
        _http.Dispose();
    }
}
