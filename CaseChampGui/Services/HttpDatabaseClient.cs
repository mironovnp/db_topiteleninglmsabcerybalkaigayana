using System;
using System.Net.Http;
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
    private readonly object _sync = new();

    private string _host = "127.0.0.1";
    private int _port = 8080;
    private ConnectionStatus _status = ConnectionStatus.Disconnected;
    private string? _currentDb;
    private string? _currentUser;
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
    public string? SessionId { get { lock (_sync) return _sessionId; } }

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
        }
        StateChanged?.Invoke(this, EventArgs.Empty);
    }

    public async Task<bool> PingAsync(CancellationToken cancellationToken = default)
    {
        Status = ConnectionStatus.Connecting;
        try
        {
            using var cts = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
            cts.CancelAfter(TimeSpan.FromSeconds(4));
            var url = $"http://{Host}:{Port}/ping";
            using var response = await _http.GetAsync(url, cts.Token).ConfigureAwait(false);
            var ok = response.IsSuccessStatusCode;
            Status = ok ? ConnectionStatus.Connected : ConnectionStatus.Failed;
            return ok;
        }
        catch
        {
            Status = ConnectionStatus.Failed;
            return false;
        }
    }

    public async Task<QueryResult> ExecuteAsync(string sql, bool dryRun = false, CancellationToken cancellationToken = default)
    {
        if (string.IsNullOrWhiteSpace(sql))
        {
            return QueryResult.Fail("Запрос пуст.");
        }

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
            if (result.CurrentDb is not null && result.CurrentDb != _currentDb)
            {
                _currentDb = result.CurrentDb;
                changed = true;
            }
            if (result.CurrentUser is not null && result.CurrentUser != _currentUser)
            {
                _currentUser = result.CurrentUser;
                changed = true;
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
        }
        if (changed)
        {
            StateChanged?.Invoke(this, EventArgs.Empty);
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

    public void Dispose() => _http.Dispose();
}
