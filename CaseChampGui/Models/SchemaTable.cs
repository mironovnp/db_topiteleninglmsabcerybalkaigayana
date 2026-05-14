using System.Collections.Generic;

namespace CaseChampGui.Models;

public sealed record SchemaColumn(string Name, string Type, string Key, bool Nullable);

public sealed record SchemaTable(string Name, IReadOnlyList<SchemaColumn> Columns);
