#include "engine/storage/storage.hpp"
#include "engine/buffer_pool.hpp"
#include "engine/btree.hpp"
#include "engine/cell_value.hpp"
#include "engine/row_codec.hpp"
#include "engine/storage/storage_internal.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>

namespace db {

using storage_i::mini_index_row_schema;
using storage_i::pack_index_leaf_row;
using storage_i::skip_index_source_cell;
using storage_i::tree_cell_lex;
using storage_i::wal_encode_btree_key;

static void rewriteMeta(BufferPool& pool, Storage* storage, const std::string& abs_path, const TableSchema& schema,
                         PageId root_id) {
    Page* meta = pool.fetchPage(0);
    std::string payload = Storage::serializeSchema(schema);
    memcpy(&payload[0], &root_id, 4);
    memcpy(meta->data + 16, payload.data(), payload.size());
    meta->setNumRecords(static_cast<uint32_t>(payload.size()));
    if (storage) storage->walLogPageImage(abs_path, 0, *meta);
    pool.unpinPage(0, true);
    if (storage) storage->walFlushDurably();
}

bool Storage::createIndex(const std::string& db_name, const std::string& table_name,
                          const std::string& index_name, const std::string& column_name) {
    if (!tableExists(db_name, table_name)) return false;

    TableSchema schema = getTableSchema(db_name, table_name);

    int col_idx = -1;
    for (int i = 0; i < (int)schema.columns.size(); ++i)
        if (schema.columns[i].name == column_name) {
            col_idx = i;
            break;
        }
    if (col_idx < 0) return false;

    for (const auto& idx : schema.indexes)
        if (idx.index_name == index_name || idx.column_name == column_name) return false;

    auto ip = indexPath(db_name, table_name, column_name);
    if (std::filesystem::exists(ip)) return false;

    auto rows = readAllRows(db_name, table_name);

    int pk_idx = schema.primary_key_index;
    TableSchema mini = mini_index_row_schema(schema, col_idx);
    BTreeBulkKeySpec ix_spec{0, 1};

    std::vector<Row> idx_rows;
    for (const auto& row : rows) {
        if (col_idx >= (int)row.size()) continue;
        if (skip_index_source_cell(row[col_idx])) continue;
        idx_rows.push_back(pack_index_leaf_row(row[col_idx], row[pk_idx]));
    }
    std::sort(idx_rows.begin(), idx_rows.end(), [&](const Row& a, const Row& b) {
        return compare_btree_keys(BPlusTree::bulkExtractKey(a, ix_spec),
                                  BPlusTree::bulkExtractKey(b, ix_spec)) < 0;
    });

    BufferPool& pool = getPool(ip.string());

    PageId meta_id;
    Page* meta = pool.newPage(&meta_id);
    meta->setPageType(META_PAGE);
    meta->setPageId(meta_id);

    PageId root_id = BPlusTree::bulkLoad(pool, idx_rows, &mini, ix_spec);

    memcpy(meta->data + 16, &root_id, 4);
    meta->setNumRecords(4);
    walLogPageImage(ip.string(), meta_id, *meta);
    pool.unpinPage(meta_id, true);
    if (wal_mgr_) wal_mgr_->flushTo(wal_mgr_->getNextLSN() - 1);
    maybeCheckpoint();

    schema.indexes.push_back({index_name, column_name});
    auto tp = tablePath(db_name, table_name);
    BufferPool& tpool = getPool(tp.string());
    Page* tmeta = tpool.fetchPage(0);
    PageId troot;
    memcpy(&troot, tmeta->data + 16, 4);
    tpool.unpinPage(0, false);
    rewriteMeta(tpool, this, tp.string(), schema, troot);

    return true;
}

bool Storage::dropIndex(const std::string& db_name, const std::string& table_name,
                        const std::string& index_name) {
    if (!tableExists(db_name, table_name)) return false;

    TableSchema schema = getTableSchema(db_name, table_name);

    int idx_pos = -1;
    for (int i = 0; i < (int)schema.indexes.size(); ++i)
        if (schema.indexes[i].index_name == index_name) {
            idx_pos = i;
            break;
        }
    if (idx_pos < 0) return false;

    std::string col_name = schema.indexes[idx_pos].column_name;

    auto ip = indexPath(db_name, table_name, col_name);
    if (std::filesystem::exists(ip)) {
        closePool(ip.string());
        std::filesystem::remove(ip);
    }

    schema.indexes.erase(schema.indexes.begin() + idx_pos);
    auto tp = tablePath(db_name, table_name);
    BufferPool& tpool = getPool(tp.string());
    Page* tmeta = tpool.fetchPage(0);
    PageId troot;
    memcpy(&troot, tmeta->data + 16, 4);
    tpool.unpinPage(0, false);
    rewriteMeta(tpool, this, tp.string(), schema, troot);

    return true;
}

std::vector<std::string> Storage::indexLookup(const std::string& db_name,
                                               const std::string& table_name,
                                               const std::string& column_name,
                                               const std::string& value) const {
    auto ip = indexPath(db_name, table_name, column_name);
    if (!std::filesystem::exists(ip)) return {};

    TableSchema schema = getTableSchema(db_name, table_name);
    int col_idx = -1;
    for (int i = 0; i < (int)schema.columns.size(); ++i)
        if (schema.columns[i].name == column_name) {
            col_idx = i;
            break;
        }
    if (col_idx < 0) return {};
    TableSchema mini = mini_index_row_schema(schema, col_idx);

    BufferPool& pool = getPool(ip.string());
    Page* meta = pool.fetchPage(0);
    PageId root_id;
    memcpy(&root_id, meta->data + 16, 4);
    pool.unpinPage(0, false);

    BPlusTree tree(pool, root_id, &mini, 2);
    BTreeKey pref{coerce_string_to_cell_column(schema.columns[col_idx], value, false)};
    auto matches = tree.scanPrefix(pref);

    std::vector<std::string> pks;
    for (const auto& row : matches) {
        if (row.size() >= 2) {
            pks.push_back(tree_cell_lex(row[1]));
        }
    }
    return pks;
}

std::vector<std::string> Storage::indexScan(const std::string& db_name,
                                             const std::string& table_name,
                                             const std::string& column_name,
                                             const std::string* low,
                                             const std::string* high) const {
    auto ip = indexPath(db_name, table_name, column_name);
    if (!std::filesystem::exists(ip)) return {};

    TableSchema schema = getTableSchema(db_name, table_name);
    int col_idx = -1;
    for (int i = 0; i < (int)schema.columns.size(); ++i)
        if (schema.columns[i].name == column_name) {
            col_idx = i;
            break;
        }
    if (col_idx < 0) return {};
    TableSchema mini = mini_index_row_schema(schema, col_idx);

    BufferPool& pool = getPool(ip.string());
    Page* meta = pool.fetchPage(0);
    PageId root_id;
    memcpy(&root_id, meta->data + 16, 4);
    pool.unpinPage(0, false);

    BPlusTree tree(pool, root_id, &mini, 2);
    std::optional<BTreeKey> low_k, high_k;
    if (low)
        low_k = BTreeKey{coerce_string_to_cell_column(schema.columns[col_idx], *low, false)};
    if (high)
        high_k = BTreeKey{coerce_string_to_cell_column(schema.columns[col_idx], *high, false)};
    auto matches = tree.scanRange(low_k, high_k);

    std::vector<std::string> pks;
    for (const auto& row : matches) {
        if (row.size() >= 2) pks.push_back(tree_cell_lex(row[1]));
    }
    return pks;
}

void Storage::indexInsertRow(const std::string& db_name, const std::string& table_name,
                             const TableSchema& schema, const Row& row) {
    int pk_idx = schema.primary_key_index;

    for (const auto& idx : schema.indexes) {
        int col_idx = -1;
        for (int i = 0; i < (int)schema.columns.size(); ++i)
            if (schema.columns[i].name == idx.column_name) {
                col_idx = i;
                break;
            }
        if (col_idx < 0 || col_idx >= (int)row.size()) continue;
        if (skip_index_source_cell(row[col_idx])) continue;

        auto ip = indexPath(db_name, table_name, idx.column_name);
        if (!std::filesystem::exists(ip)) continue;

        TableSchema mini = mini_index_row_schema(schema, col_idx);
        Row idx_row = pack_index_leaf_row(row[col_idx], row[pk_idx]);
        walAppendRowUpsert(ip.string(), wal_encode_btree_key({row[col_idx], row[pk_idx]}),
                           serialize_row_disk(mini, idx_row));
    }
    walFlushDurably();

    for (const auto& idx : schema.indexes) {
        int col_idx = -1;
        for (int i = 0; i < (int)schema.columns.size(); ++i)
            if (schema.columns[i].name == idx.column_name) {
                col_idx = i;
                break;
            }
        if (col_idx < 0 || col_idx >= (int)row.size()) continue;
        if (skip_index_source_cell(row[col_idx])) continue;

        auto ip = indexPath(db_name, table_name, idx.column_name);
        if (!std::filesystem::exists(ip)) continue;

        TableSchema mini = mini_index_row_schema(schema, col_idx);

        BufferPool& pool = getPool(ip.string());
        Page* meta = pool.fetchPage(0);
        PageId root_id;
        memcpy(&root_id, meta->data + 16, 4);
        pool.unpinPage(0, false);

        Row idx_row = pack_index_leaf_row(row[col_idx], row[pk_idx]);

        BPlusTree tree(pool, root_id, &mini, 2);
        tree.upsert({row[col_idx], row[pk_idx]}, idx_row);

        PageId new_root = tree.getRootPageId();
        meta = pool.fetchPage(0);
        memcpy(meta->data + 16, &new_root, 4);
        pool.unpinPage(0, true);
    }

    walFlushDurably();
    maybeCheckpoint();
}

bool Storage::hasIndex(const std::string& db_name, const std::string& table_name,
                       const std::string& column_name) const {
    auto ip = indexPath(db_name, table_name, column_name);
    return std::filesystem::exists(ip);
}

} // namespace db
