using System.Threading;
using System.Threading.Tasks;

namespace CaseChampGui.Services;

public sealed record ColumnFilterTranslateResult(bool Success, string? Condition, string Message);

public interface IColumnFilterTranslator
{
    bool IsEnabled { get; }
    Task<ColumnFilterTranslateResult> TranslateAsync(
        string tableName,
        string columnName,
        string columnType,
        string russianText,
        CancellationToken cancellationToken = default);
}
