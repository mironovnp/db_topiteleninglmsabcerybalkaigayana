namespace CaseChampGui.Models;

public enum ColumnFilterKind
{
    Compare,
    Text2Sql,
}

public sealed class ColumnFilter
{
    public required ColumnFilterKind Kind { get; init; }
    public required string ColumnName { get; init; }
    public string ColumnType { get; init; } = "TEXT";
    public string? CompareOperator { get; init; }
    public string? CompareValue { get; init; }
    public string? WhereFragment { get; init; }

    public string DisplayText => Kind switch
    {
        ColumnFilterKind.Compare =>
            $"{ColumnName} {CompareOperator} {CompareValue}",
        ColumnFilterKind.Text2Sql =>
            $"{ColumnName}: {WhereFragment}",
        _ => ColumnName,
    };
}
