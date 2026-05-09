#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace db {

struct ColumnDef;

using CellPrimitive = std::variant<int64_t, double, bool, std::string>;
/// SQL NULL encoded as empty optional (distinct from typed empty string TEXT).
using CellValue = std::optional<CellPrimitive>;
using Row = std::vector<CellValue>;
/// B+-tree key: one cell (clustered PK) or two cells (secondary: indexed column + PK).
using BTreeKey = std::vector<CellValue>;

/// Total order on scalars (NULL first). Same-type compares use native operators; mixed types use lexical fallback.
int compare_cell_values(const CellValue& a, const CellValue& b);

/// Lexicographic compare over key components.
int compare_btree_keys(const BTreeKey& a, const BTreeKey& b);

/// Compare for B+tree navigation / range bounds: if one key is a prefix of the other in length, they compare equal.
int compare_btree_keys_nav(const BTreeKey& a, const BTreeKey& b);

extern const uint8_t kRowMagic[4];

std::string cell_primitive_to_lexical_for_key(const CellPrimitive& p);
CellPrimitive parse_numeric_literal_strict(const ColumnDef& col, const std::string& literal,
                                           bool quoted);
CellValue coerce_string_to_cell_column(const ColumnDef& col, const std::string& literal,
                                       bool literal_was_quoted);

/// Migrate legacy serialized string-rows (page.hpp v1) to typed Row using column defs.
Row legacy_strings_to_cells(const ColumnDef* columns, std::size_t ncols,
                            const std::vector<std::string>& legacy_strings);

inline std::string cell_to_where_string(CellValue const& c) {
    if (!c) return "";
    return cell_primitive_to_lexical_for_key(*c);
}

inline bool rows_pk_equal(size_t pk_idx, const Row& a, const Row& b) {
    if (pk_idx >= a.size() || pk_idx >= b.size()) return false;
    return cell_to_where_string(a[pk_idx]) == cell_to_where_string(b[pk_idx]);
}

} // namespace db
