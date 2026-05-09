#include "engine/storage/storage_internal.hpp"
#include "engine/row_codec.hpp"

namespace db {
namespace storage_i {

TableSchema mini_index_row_schema(const TableSchema& sch, int col_idx) {
    TableSchema x;
    x.table_name = sch.table_name;
    x.columns.push_back(sch.columns[col_idx]);
    x.columns.push_back(sch.columns[sch.primary_key_index]);
    x.primary_key_index = 1;
    return x;
}

std::string tree_cell_lex(const CellValue& cv) {
    if (!cv.has_value())
        return "";
    return cell_primitive_to_lexical_for_key(*cv);
}

bool skip_index_source_cell(const CellValue& c) {
    return cell_to_where_string(c).empty();
}

Row pack_index_leaf_row(const CellValue& col_cell, const CellValue& pk_cell) {
    Row ir;
    ir.push_back(col_cell);
    ir.push_back(pk_cell);
    return ir;
}

BTreeKey cluster_key_from_literal(const TableSchema& s, const std::string& key_lit) {
    if (s.columns.empty() || s.primary_key_index < 0 ||
        s.primary_key_index >= static_cast<int>(s.columns.size()))
        return {std::nullopt};
    return {coerce_string_to_cell_column(s.columns[static_cast<size_t>(s.primary_key_index)], key_lit,
                                          false)};
}

BTreeKey cluster_key_from_row(const TableSchema& s, const Row& row) {
    int pk = s.primary_key_index;
    CellValue v = (pk >= 0 && pk < static_cast<int>(row.size())) ? row[static_cast<size_t>(pk)]
                                                                 : std::nullopt;
    return {v};
}

std::string wal_encode_btree_key(const BTreeKey& k) {
    std::string buf;
    btree_key_append_bytes(buf, k);
    return buf;
}

} // namespace storage_i
} // namespace db
