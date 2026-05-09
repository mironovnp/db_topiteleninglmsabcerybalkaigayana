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

/// One cell in TBW2 wire format: 0 = NULL, else tag + payload (same as in row body).
void btree_append_cell(std::string& buf, CellValue cv);

/// Reads one cell; advances p; returns false on truncation / bad tag.
bool btree_read_cell(const uint8_t*& p, const uint8_t* end, CellValue& out);

/// B+-tree / WAL key: concatenation of TBW2 cells (no extra framing).
void btree_key_append_bytes(std::string& buf, const BTreeKey& key);

/// Strict decode: exactly `arity` cells and consumes all bytes.
bool btree_key_parts_from_bytes(const uint8_t* data, size_t len, uint8_t arity, BTreeKey& out);

/// Pre-typed-key legacy: UTF-8 lexical key or "indexed\\0pk" composite string.
bool btree_key_from_legacy_bytes(const uint8_t* data, size_t len, uint8_t arity, const TableSchema& sch,
                                 BTreeKey& out);

bool decode_btree_key_blob(const uint8_t* data, uint32_t len, uint8_t arity, const TableSchema& sch,
                           BTreeKey& out);

} // namespace db
