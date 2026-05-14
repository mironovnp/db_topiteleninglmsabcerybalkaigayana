using System;
using System.Threading;
using System.Threading.Tasks;
using CaseChampGui.Models;

namespace CaseChampGui.Services;

public enum ConnectionStatus
{
    Disconnected,
    Connecting,
    Connected,
    Failed,
}

public interface IDatabaseClient
{
    ConnectionStatus Status { get; }
    string Host { get; }
    int Port { get; }
    string? CurrentDb { get; }
    string? CurrentUser { get; }
    string? SessionId { get; }

    event EventHandler? StateChanged;

    void Configure(string host, int port);
    Task<bool> PingAsync(CancellationToken cancellationToken = default);
    Task<QueryResult> ExecuteAsync(string sql, bool dryRun = false, CancellationToken cancellationToken = default);

    void ClearSession();

    Task LogoutAsync(CancellationToken cancellationToken = default);
}
