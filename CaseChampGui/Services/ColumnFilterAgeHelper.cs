using System;
using System.Globalization;
using System.Text.RegularExpressions;

namespace CaseChampGui.Services;

/// <summary>
/// Builds simple date comparisons for Russian "older/younger than N years" without Mistral.
/// </summary>
public static class ColumnFilterAgeHelper
{
    private static readonly Regex AgePattern = new(
        @"(?<op>старше|моложе|младше|older\s+than|younger\s+than)\s+(?<n>\d{1,3})\s*(?<unit>лет|год|года|years|year)?",
        RegexOptions.IgnoreCase | RegexOptions.CultureInvariant);

    public static bool TryBuildCondition(string columnName, string russianText, out string? condition)
    {
        condition = null;
        if (string.IsNullOrWhiteSpace(columnName) || string.IsNullOrWhiteSpace(russianText))
            return false;

        var m = AgePattern.Match(russianText.Trim());
        if (!m.Success)
            return false;

        if (!int.TryParse(m.Groups["n"].Value, NumberStyles.Integer, CultureInfo.InvariantCulture, out var years)
            || years is < 1 or > 150)
        {
            return false;
        }

        var cutoff = DateTime.Today.AddYears(-years).ToString("yyyy-MM-dd", CultureInfo.InvariantCulture);
        var col = SanitizeColumn(columnName);
        var op = m.Groups["op"].Value.ToLowerInvariant();

        condition = op switch
        {
            "старше" or "older than" => $"{col} < '{cutoff}'",
            "моложе" or "младше" or "younger than" => $"{col} > '{cutoff}'",
            _ => null,
        };

        return condition is not null;
    }

    private static string SanitizeColumn(string name)
        => name.Replace("`", string.Empty, StringComparison.Ordinal)
            .Replace(";", string.Empty, StringComparison.Ordinal)
            .Trim();
}
