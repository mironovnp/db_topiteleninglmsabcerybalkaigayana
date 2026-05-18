using System.Threading;
using System.Threading.Tasks;

namespace CaseChampGui.Services;

public sealed record Text2SqlResult(bool Success, string? Sql, string Message);

public interface IText2SqlService
{
    bool IsEnabled { get; }
    Task<Text2SqlResult> TranslateAsync(
        string russianText,
        string? activeDatabase = null,
        CancellationToken cancellationToken = default);
}
