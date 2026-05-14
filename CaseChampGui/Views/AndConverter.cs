using System.Collections.Generic;
using System.Globalization;
using Avalonia.Data.Converters;

namespace CaseChampGui.Views;

public sealed class AndConverter : IMultiValueConverter
{
    public static readonly AndConverter Instance = new();

    public object Convert(IList<object?> values, System.Type targetType, object? parameter, CultureInfo culture)
    {
        foreach (var v in values)
        {
            if (v is bool b)
            {
                if (!b) return false;
            }
            else if (v is null)
            {
                return false;
            }
        }
        return true;
    }
}
