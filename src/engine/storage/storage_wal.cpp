#include "engine/storage/storage.hpp"
#include "engine/buffer_pool.hpp"
#include "engine/btree.hpp"
#include "engine/row_codec.hpp"
#include "engine/storage/storage_internal.hpp"

#include <cstring>

namespace db {

namespace {

constexpr uint64_t WAL_CHECKPOINT_THRESHOLD_BYTES = 4ull * 1024 * 1024; // 4 MiB

bool pathEndsWithDb(const std::string& path) {
    return path.size() >= 3 && path.compare(path.size() - 3, 3, ".db") == 0;
}

bool pathEndsWithIdx(const std::string& path) {
    return path.size() >= 4 && path.compare(path.size() - 4, 4, ".idx") == 0;
}

} // namespace

using storage_i::cluster_key_from_row;
using storage_i::mini_index_row_schema;
using storage_i::skip_index_source_cell;
using storage_i::pack_index_leaf_row;
using storage_i::wal_encode_btree_key;

void Storage::maybeCheckpoint() {
    if (!wal_mgr_) return;
    if (wal_mgr_->fileSizeBytes() < WAL_CHECKPOINT_THRESHOLD_BYTES) return;

    wal_mgr_->flushTo(wal_mgr_->getNextLSN() - 1);
    flushAllPools();
    wal_mgr_->reset();
}

void Storage::walAppendRowDelete(const std::string& abs_path, const std::string& key) {
    if (!wal_mgr_) return;
    LogRecord r(0, 0, LogRecordType::ROW_DELETE, 0, LogRecord::encodeRowPayload(abs_path, key));
    wal_mgr_->appendRecord(r);
}

void Storage::walAppendRowUpsert(const std::string& abs_path, const std::string& key,
                                 const std::string& row_blob) {
    if (!wal_mgr_) return;
    LogRecord r(0, 0, LogRecordType::ROW_UPSERT, 0,
               LogRecord::encodeRowPayload(abs_path, key, row_blob));
    wal_mgr_->appendRecord(r);
}

void Storage::walFlushDurably() {
    if (wal_mgr_) wal_mgr_->flushTo(wal_mgr_->getNextLSN() - 1);
}

void Storage::replayWalLogicalRecord(LogRecordType type, std::string abs_path, std::string key,
                                     std::string row_blob) {
    if (!pathEndsWithDb(abs_path) && !pathEndsWithIdx(abs_path)) return;

    BufferPool replayPool(abs_path, POOL_SIZE, nullptr, true);

    if (pathEndsWithIdx(abs_path)) {
        std::filesystem::path fp(abs_path);
        std::string db_name = fp.parent_path().filename().string();
        std::string stem = fp.stem().string();
        size_t dot = stem.find('.');
        if (dot == std::string::npos) return;
        std::string table_name = stem.substr(0, dot);
        std::string column_name = stem.substr(dot + 1);

        auto tp = tablePath(db_name, table_name);
        BufferPool schemaPool(tp.string(), POOL_SIZE, nullptr, true);
        Page* sm = schemaPool.fetchPage(0);
        uint32_t slen = sm->getNumRecords();
        TableSchema sch = deserializeSchema(sm->data + 16, slen);
        schemaPool.unpinPage(0, false);

        int col_idx = -1;
        for (int i = 0; i < (int)sch.columns.size(); ++i)
            if (sch.columns[i].name == column_name) {
                col_idx = i;
                break;
            }
        if (col_idx < 0) return;
        TableSchema mini = mini_index_row_schema(sch, col_idx);

        Page* meta = replayPool.fetchPage(0);
        PageId root_id;
        memcpy(&root_id, meta->data + 16, 4);
        replayPool.unpinPage(0, false);

        BPlusTree tree(replayPool, root_id, &mini, 2);
        BTreeKey bkey;
        if (!decode_btree_key_blob(reinterpret_cast<const uint8_t*>(key.data()),
                                   static_cast<uint32_t>(key.size()), 2, mini, bkey))
            return;
        if (type == LogRecordType::ROW_DELETE) {
            tree.remove(bkey);
        } else if (type == LogRecordType::ROW_UPSERT) {
            Row rw;
            if (deserialize_row_disk(mini,
                                    reinterpret_cast<const uint8_t*>(row_blob.data()),
                                    static_cast<uint32_t>(row_blob.size()),
                                    rw) &&
                !rw.empty()) {
                tree.upsert(bkey, rw);
            }
        }
        PageId new_root = tree.getRootPageId();
        meta = replayPool.fetchPage(0);
        memcpy(meta->data + 16, &new_root, 4);
        replayPool.unpinPage(0, true);
        replayPool.flushAll();
        return;
    }

    Page* meta = replayPool.fetchPage(0);
    PageId root_id;
    memcpy(&root_id, meta->data + 16, 4);
    uint32_t payload_len = meta->getNumRecords();
    TableSchema s = deserializeSchema(meta->data + 16, payload_len);
    replayPool.unpinPage(0, false);

    BPlusTree tree(replayPool, root_id, &s, 1);
    BTreeKey bkey;
    if (!decode_btree_key_blob(reinterpret_cast<const uint8_t*>(key.data()),
                               static_cast<uint32_t>(key.size()), 1, s, bkey))
        return;
    if (type == LogRecordType::ROW_DELETE) {
        tree.remove(bkey);
    } else if (type == LogRecordType::ROW_UPSERT) {
        Row rw;
        if (deserialize_row_disk(s,
                                reinterpret_cast<const uint8_t*>(row_blob.data()),
                                static_cast<uint32_t>(row_blob.size()),
                                rw) &&
            !rw.empty()) {
            tree.upsert(bkey, rw);
        }
    }
    PageId new_root = tree.getRootPageId();
    meta = replayPool.fetchPage(0);
    memcpy(meta->data + 16, &new_root, 4);
    replayPool.unpinPage(0, true);
    replayPool.flushAll();
}

void Storage::indexRemovePhysical(const std::string& db_name, const std::string& table_name,
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

        BufferPool& pool = getPool(ip.string());
        Page* meta = pool.fetchPage(0);
        PageId root_id;
        memcpy(&root_id, meta->data + 16, 4);
        pool.unpinPage(0, false);

        BPlusTree tree(pool, root_id, &mini, 2);
        BTreeKey ik = {row[col_idx], row[pk_idx]};
        tree.remove(ik);

        PageId new_root = tree.getRootPageId();
        meta = pool.fetchPage(0);
        memcpy(meta->data + 16, &new_root, 4);
        pool.unpinPage(0, true);
    }
}

void Storage::deleteClusterRowWal(const std::string& db_name, const std::string& table_name,
                                  const TableSchema& schema, const Row& row) {
    const int pk_idx = schema.primary_key_index;
    const std::string tpath = tablePath(db_name, table_name).string();

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

        std::string idx_key_wire = wal_encode_btree_key({row[col_idx], row[pk_idx]});
        walAppendRowDelete(ip.string(), idx_key_wire);
    }

    walAppendRowDelete(tpath, wal_encode_btree_key(cluster_key_from_row(schema, row)));
    walFlushDurably();

    indexRemovePhysical(db_name, table_name, schema, row);

    BufferPool& pool = getPool(tpath);
    Page* meta = pool.fetchPage(0);
    PageId root_id;
    memcpy(&root_id, meta->data + 16, 4);
    uint32_t payload_len = meta->getNumRecords();
    TableSchema s = deserializeSchema(meta->data + 16, payload_len);
    pool.unpinPage(0, false);

    BPlusTree tree(pool, root_id, &s, 1);
    tree.remove(cluster_key_from_row(s, row));

    PageId new_root = tree.getRootPageId();
    meta = pool.fetchPage(0);
    memcpy(meta->data + 16, &new_root, 4);
    pool.unpinPage(0, true);

    walFlushDurably();
    maybeCheckpoint();
}

void Storage::upsertClusterRowWal(const std::string& db_name, const std::string& table_name,
                                  const TableSchema& schema, const Row* old_row,
                                  const Row& new_row) {
    const int pk_idx = schema.primary_key_index;
    const std::string tpath = tablePath(db_name, table_name).string();

    if (old_row) {
        for (const auto& idx : schema.indexes) {
            int col_idx = -1;
            for (int i = 0; i < (int)schema.columns.size(); ++i)
                if (schema.columns[i].name == idx.column_name) {
                    col_idx = i;
                    break;
                }
            if (col_idx < 0 || col_idx >= (int)old_row->size()) continue;
            if (skip_index_source_cell((*old_row)[col_idx])) continue;

            auto ip = indexPath(db_name, table_name, idx.column_name);
            if (!std::filesystem::exists(ip)) continue;

            walAppendRowDelete(ip.string(),
                               wal_encode_btree_key({(*old_row)[col_idx], (*old_row)[pk_idx]}));
        }

        if (compare_btree_keys(cluster_key_from_row(schema, *old_row),
                               cluster_key_from_row(schema, new_row)) != 0)
            walAppendRowDelete(tpath, wal_encode_btree_key(cluster_key_from_row(schema, *old_row)));
    }

    walAppendRowUpsert(tpath, wal_encode_btree_key(cluster_key_from_row(schema, new_row)),
                       serialize_row_disk(schema, new_row));

    for (const auto& idx : schema.indexes) {
        int col_idx = -1;
        for (int i = 0; i < (int)schema.columns.size(); ++i)
            if (schema.columns[i].name == idx.column_name) {
                col_idx = i;
                break;
            }
        if (col_idx < 0 || col_idx >= (int)new_row.size()) continue;
        if (skip_index_source_cell(new_row[col_idx])) continue;

        auto ip = indexPath(db_name, table_name, idx.column_name);
        if (!std::filesystem::exists(ip)) continue;

        TableSchema mini = mini_index_row_schema(schema, col_idx);
        Row idx_row = pack_index_leaf_row(new_row[col_idx], new_row[pk_idx]);
        walAppendRowUpsert(ip.string(), wal_encode_btree_key({new_row[col_idx], new_row[pk_idx]}),
                           serialize_row_disk(mini, idx_row));
    }

    walFlushDurably();

    if (old_row) indexRemovePhysical(db_name, table_name, schema, *old_row);

    BufferPool& pool = getPool(tpath);
    Page* meta = pool.fetchPage(0);
    PageId root_id;
    memcpy(&root_id, meta->data + 16, 4);
    uint32_t payload_len = meta->getNumRecords();
    TableSchema s = deserializeSchema(meta->data + 16, payload_len);
    pool.unpinPage(0, false);

    BPlusTree tree(pool, root_id, &s, 1);
    if (old_row) {
        BTreeKey old_k = cluster_key_from_row(s, *old_row);
        BTreeKey new_k = cluster_key_from_row(s, new_row);
        if (compare_btree_keys(old_k, new_k) != 0)
            tree.remove(old_k);
    }

    tree.upsert(cluster_key_from_row(s, new_row), new_row);

    PageId new_root = tree.getRootPageId();
    meta = pool.fetchPage(0);
    memcpy(meta->data + 16, &new_root, 4);
    pool.unpinPage(0, true);

    for (const auto& idx : schema.indexes) {
        int col_idx = -1;
        for (int i = 0; i < (int)schema.columns.size(); ++i)
            if (schema.columns[i].name == idx.column_name) {
                col_idx = i;
                break;
            }
        if (col_idx < 0 || col_idx >= (int)new_row.size()) continue;
        if (skip_index_source_cell(new_row[col_idx])) continue;

        auto ip = indexPath(db_name, table_name, idx.column_name);
        if (!std::filesystem::exists(ip)) continue;

        TableSchema mini = mini_index_row_schema(s, col_idx);

        BufferPool& ipool = getPool(ip.string());
        Page* im = ipool.fetchPage(0);
        PageId iroot;
        memcpy(&iroot, im->data + 16, 4);
        ipool.unpinPage(0, false);

        Row idx_row = pack_index_leaf_row(new_row[col_idx], new_row[pk_idx]);
        BPlusTree itree(ipool, iroot, &mini, 2);
        itree.upsert({new_row[col_idx], new_row[pk_idx]}, idx_row);

        PageId nr = itree.getRootPageId();
        im = ipool.fetchPage(0);
        memcpy(im->data + 16, &nr, 4);
        ipool.unpinPage(0, true);
    }

    walFlushDurably();
    maybeCheckpoint();
}

} // namespace db
