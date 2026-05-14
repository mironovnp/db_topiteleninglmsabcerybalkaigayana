using Avalonia;
using Avalonia.Controls;
using CaseChampGui.Models;

namespace CaseChampGui.Views;

public partial class ResultTable : UserControl
{
    public static readonly StyledProperty<QueryResult?> ResultProperty =
        AvaloniaProperty.Register<ResultTable, QueryResult?>(nameof(Result));

    public QueryResult? Result
    {
        get => GetValue(ResultProperty);
        set => SetValue(ResultProperty, value);
    }

    public ResultTable()
    {
        InitializeComponent();
        PropertyChanged += (_, e) =>
        {
            if (e.Property == ResultProperty) Rebuild();
        };
        ActualThemeVariantChanged += (_, _) => Rebuild();
    }

    private void Rebuild()
    {
        var host = this.FindControl<Grid>("TableHost");
        if (host is null) return;

        var r = Result;
        if (r is null || !r.Success || r.Columns.Count == 0)
        {
            host.RowDefinitions.Clear();
            host.ColumnDefinitions.Clear();
            host.Children.Clear();
            return;
        }

        ResultTableBuilder.Build(host, r.Columns, r.Rows.ConvertAll(row => (System.Collections.Generic.IList<string>)row));
    }
}
