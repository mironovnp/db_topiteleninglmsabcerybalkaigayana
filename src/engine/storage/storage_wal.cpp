#include "engine/storage/storage.hpp"
#include "engine/buffer_pool.hpp"
#include "engine/btree.hpp"
#include "engine/row_codec.hpp"
#include "engine/storage/storage_internal.hpp"

#include <cstring>
#include <stdexcept>

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
    if (txn_active_) return;
    if (wal_mgr_->fileSizeBytes() < WAL_CHECKPOINT_THRESHOLD_BYTES) return;

    wal_mgr_->flushTo(wal_mgr_->getNextLSN() - 1);
    flushAllPools();
    wal_mgr_->flushTo(wal_mgr_->getNextLSN() - 1);
    wal_mgr_->reset();
}

LSN Storage::walAppendRecord(LogRecord record) {
    if (!wal_mgr_) return INVALID_LSN;
    if (txn_active_) {
        record.txn_id = current_txn_id_;
        record.prev_lsn = current_txn_prev_lsn_;
    }
    LSN lsn = wal_mgr_->appendRecord(record);
    if (txn_active_) current_txn_prev_lsn_ = lsn;
    return lsn;
}

void Storage::walAppendRowDelete(const std::string& abs_path, const std::string& key,
                                 const std::string& old_row_blob) {
    if (!wal_mgr_) return;
    LogRecord r(0, 0, LogRecordType::ROW_DELETE, 0,
                LogRecord::encodeRowPayload(abs_path, key, old_row_blob));
    walAppendRecord(std::move(r));
}

void Storage::walAppendRowUpsert(const std::string& abs_path, const std::string& key,
                                 const std::string& row_blob) {
    if (!wal_mgr_) return;
    LogRecord r(0, 0, LogRecordType::ROW_UPSERT, 0,
               LogRecord::encodeRowPayload(abs_path, key, row_blob));
    walAppendRecord(std::move(r));
}

void Storage::walFlushDurably() {
    if (wal_mgr_) wal_mgr_->flushTo(wal_mgr_->getNextLSN() - 1);
}
    
void Storage::walLogPageImage(const std::string& abs_path, PageId page_id, const Page& pg) {
    if (!wal_mgr_) return;
    std::string payload;
    uint16_t fpl = static_cast<uint16_t>(abs_path.size());
    payload.append(reinterpret_cast<const char*>(&fpl), 2);
    payload.append(abs_path);
    payload.append(reinterpret_cast<const char*>(pg.data), PAGE_SIZE);
    
    LogRecord rec(0, 0, LogRecordType::PAGE_IMAGE, page_id, std::move(payload));
    walAppendRecord(std::move(rec));
}

bool Storage::transactionActive() const {
    return txn_active_;
}

void Storage::beginTransaction() {
    if (txn_active_) throw std::runtime_error("Transaction already active");
    txn_active_ = true;
    current_txn_id_ = next_txn_id_.fetch_add(1);
    current_txn_prev_lsn_ = INVALID_LSN;
    LogRecord begin(current_txn_id_, INVALID_LSN, LogRecordType::BEGIN_TXN, 0);
    walAppendRecord(std::move(begin));
}

void Storage::commitTransaction() {
    if (!txn_active_) throw std::runtime_error("No active transaction");
    LogRecord commit(current_txn_id_, current_txn_prev_lsn_, LogRecordType::COMMIT_TXN, 0);
    walAppendRecord(std::move(commit));
    walFlushDurably();
    txn_active_ = false;
    current_txn_id_ = 0;
    current_txn_prev_lsn_ = INVALID_LSN;
    maybeCheckpoint();
}

void Storage::rollbackTransaction() {
    if (!txn_active_) throw std::runtime_error("No active transaction");
    if (!wal_mgr_) return;

    walFlushDurably();
    auto records = wal_mgr_->readAllRecords();
    const TxnId txn_id = current_txn_id_;

    for (auto it = records.rbegin(); it != records.rend(); ++it) {
        const LogRecord& rec = *it;
        if (rec.txn_id != txn_id) continue;
        if (rec.type == LogRecordType::BEGIN_TXN) break;
        if (rec.type == LogRecordType::COMMIT_TXN || rec.type == LogRecordType::ABORT_TXN ||
            rec.type == LogRecordType::CLR_ROW_UPSERT || rec.type == LogRecordType::CLR_ROW_DELETE) {
            continue;
        }
        if (rec.type != LogRecordType::ROW_UPSERT && rec.type != LogRecordType::ROW_DELETE) {
            continue;
        }

        std::string path, key, row_blob;
        if (!LogRecord::decodeRowPayload(rec.payload, path, key, row_blob)) continue;

        LogRecordType clr_type = LogRecordType::CLR_ROW_DELETE;
        std::string clr_blob;
        if (rec.type == LogRecordType::ROW_DELETE) {
            if (row_blob.empty()) continue;
            clr_type = LogRecordType::CLR_ROW_UPSERT;
            clr_blob = row_blob;
        }

        LogRecord clr(txn_id, current_txn_prev_lsn_, clr_type, 0,
                      LogRecord::encodeRowPayload(path, key, clr_blob));
        walAppendRecord(std::move(clr));
        walFlushDurably();

        replayWalLogicalRecord(clr_type == LogRecordType::CLR_ROW_UPSERT ? LogRecordType::ROW_UPSERT
                                                                         : LogRecordType::ROW_DELETE,
                               path, key, clr_blob);
    }

    LogRecord abort(txn_id, current_txn_prev_lsn_, LogRecordType::ABORT_TXN, 0);
    walAppendRecord(std::move(abort));
    walFlushDurably();

    txn_active_ = false;
    current_txn_id_ = 0;
    current_txn_prev_lsn_ = INVALID_LSN;
    maybeCheckpoint();
}

void Storage::replayWalLogicalRecord(LogRecordType type, std::string abs_path, std::string key,
                                     std::string row_blob) {
    if (!pathEndsWithDb(abs_path) && !pathEndsWithIdx(abs_path)) return;
    if (!walReplayDataFileReady(abs_path)) return;

    BufferPool& replayPool = getPool(abs_path);

    if (pathEndsWithIdx(abs_path)) {
        std::filesystem::path fp(abs_path);
        std::string db_name = fp.parent_path().filename().string();
        std::string stem = fp.stem().string();
        size_t dot = stem.find('.');
        if (dot == std::string::npos) return;
        std::string table_name = stem.substr(0, dot);
        std::string column_name = stem.substr(dot + 1);

        auto tp = tablePath(db_name, table_name);
        if (!walReplayDataFileReady(tp)) return;
        BufferPool& schemaPool = getPool(tp.string());
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
        if (type == LogRecordType::ROW_DELETE || type == LogRecordType::CLR_ROW_DELETE) {
            tree.remove(bkey);
        } else if (type == LogRecordType::ROW_UPSERT || type == LogRecordType::CLR_ROW_UPSERT) {
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
    if (type == LogRecordType::ROW_DELETE || type == LogRecordType::CLR_ROW_DELETE) {
        tree.remove(bkey);
    } else if (type == LogRecordType::ROW_UPSERT || type == LogRecordType::CLR_ROW_UPSERT) {
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

        TableSchema mini = mini_index_row_schema(schema, col_idx);
        Row idx_row = pack_index_leaf_row(row[col_idx], row[pk_idx]);
        std::string idx_key_wire = wal_encode_btree_key({row[col_idx], row[pk_idx]});
        walAppendRowDelete(ip.string(), idx_key_wire, serialize_row_disk(mini, idx_row));
    }

    walAppendRowDelete(tpath, wal_encode_btree_key(cluster_key_from_row(schema, row)),
                       serialize_row_disk(schema, row));
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
    walLogPageImage(tpath, 0, *meta);
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

            TableSchema mini = mini_index_row_schema(schema, col_idx);
            Row idx_row = pack_index_leaf_row((*old_row)[col_idx], (*old_row)[pk_idx]);
            walAppendRowDelete(ip.string(),
                               wal_encode_btree_key({(*old_row)[col_idx], (*old_row)[pk_idx]}),
                               serialize_row_disk(mini, idx_row));
        }

        walAppendRowDelete(tpath, wal_encode_btree_key(cluster_key_from_row(schema, *old_row)),
                           serialize_row_disk(schema, *old_row));
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
    walLogPageImage(tpath, 0, *meta);
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
        walLogPageImage(ip.string(), 0, *im);
        ipool.unpinPage(0, true);
    }

    walFlushDurably();
    maybeCheckpoint();
}

} // namespace db
