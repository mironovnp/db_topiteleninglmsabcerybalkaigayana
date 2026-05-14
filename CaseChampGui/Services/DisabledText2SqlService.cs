using System.Threading;
using System.Threading.Tasks;

namespace CaseChampGui.Services;

public sealed class DisabledText2SqlService : IText2SqlService
{
    public bool IsEnabled => false;

    public Task<Text2SqlResult> TranslateAsync(string russianText, CancellationToken cancellationToken = default)
        => Task.FromResult(new Text2SqlResult(
            Success: false,
            Sql: null,
            Message: "Text2SQL ещё не подключён. Раздел появится здесь после интеграции серверного шлюза."));
}
