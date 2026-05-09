#pragma once

#include "engine/storage/storage.hpp"
#include "engine/cell_value.hpp"

namespace db {
namespace storage_i {

TableSchema mini_index_row_schema(const TableSchema& sch, int col_idx);
std::string     tree_cell_lex(const CellValue& cv);
bool            skip_index_source_cell(const CellValue& c);
Row             pack_index_leaf_row(const CellValue& col_cell, const CellValue& pk_cell);
BTreeKey        cluster_key_from_literal(const TableSchema& s, const std::string& key_lit);
BTreeKey        cluster_key_from_row(const TableSchema& s, const Row& row);
std::string     wal_encode_btree_key(const BTreeKey& k);

} // namespace storage_i
} // namespace db
