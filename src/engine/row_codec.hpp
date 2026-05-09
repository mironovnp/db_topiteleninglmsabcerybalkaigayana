#pragma once

#include "engine/cell_value.hpp"

#include <cstdint>
#include <string>

namespace db {

struct TableSchema;

enum class StoredCellTag : uint8_t { Int = 1, Float = 2, Bool = 3, Text = 4 };

/// Binary row blob v2 ('TBW2'+payload). Fallback: legacy UTF-16 count string row (v1).
std::string serialize_row_disk(const TableSchema& schema, const Row& row);

/// Parses v2 or migrates legacy v1 blob to typed Row using schema columns.
bool deserialize_row_disk(const TableSchema& schema, const uint8_t* blob, uint32_t len,
                         Row& out);

} // namespace db
