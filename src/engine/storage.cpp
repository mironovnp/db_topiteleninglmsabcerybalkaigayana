#include "engine/storage.hpp"
#include "engine/buffer_pool.hpp"
#include "engine/btree.hpp"
#include "engine/cell_value.hpp"
#include "engine/row_codec.hpp"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace db {

namespace {

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

} // namespace

static constexpr uint64_t WAL_CHECKPOINT_THRESHOLD_BYTES = 4ull * 1024 * 1024; // 4 MiB

Storage::Storage(const std::string& data_dir) : data_dir_(data_dir) {
    std::filesystem::create_directories(data_dir_);
    wal_mgr_ = std::make_unique<WALManager>((data_dir_ / "wal.log").string());
    // Crash recovery: replay WAL into data files before opening any BufferPools.
    wal_mgr_->recover(this);
}

void Storage::flushAllPools() const {
    for (auto& [_, pool] : pools_) {
        if (pool) pool->flushAll();
    }
}

void Storage::maybeCheckpoint() {
    if (!wal_mgr_) return;
    if (wal_mgr_->fileSizeBytes() < WAL_CHECKPOINT_THRESHOLD_BYTES) return;

    // Checkpoint protocol (simple No-Force version):
    // 1) Force WAL (everything appended so far) to disk.
    wal_mgr_->flushTo(wal_mgr_->getNextLSN() - 1);
    // 2) Flush all dirty pages to disk.
    flushAllPools();
    // 3) Reset WAL to empty.
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

static bool pathEndsWithDb(const std::string& path) {
    return path.size() >= 3 && path.compare(path.size() - 3, 3, ".db") == 0;
}

static bool pathEndsWithIdx(const std::string& path) {
    return path.size() >= 4 && path.compare(path.size() - 4, 4, ".idx") == 0;
}

void Storage::replayWalLogicalRecord(LogRecordType type, std::string abs_path,
                                    std::string key, std::string row_blob) {
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

    // Clustered .db table
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

    // Secondary indexes physical insert/update
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

std::filesystem::path Storage::dbPath(const std::string& db) const {
    return data_dir_ / db;
}

std::filesystem::path Storage::tablePath(const std::string& db,
                                          const std::string& tbl) const {
    return data_dir_ / db / (tbl + ".db");
}

std::filesystem::path Storage::indexPath(const std::string& db,
                                          const std::string& tbl,
                                          const std::string& col) const {
    return data_dir_ / db / (tbl + "." + col + ".idx");
}

// ── Database ops ───────────────────────────────────────────────────────

bool Storage::createDatabase(const std::string& db_name) {
    auto p = dbPath(db_name);
    if (std::filesystem::exists(p)) return false;
    return std::filesystem::create_directories(p);
}

bool Storage::dropDatabase(const std::string& db_name) {
    auto p = dbPath(db_name);
    if (!std::filesystem::exists(p)) return false;
    std::filesystem::remove_all(p);
    return true;
}

bool Storage::databaseExists(const std::string& db_name) const {
    return std::filesystem::is_directory(dbPath(db_name));
}

std::vector<std::string> Storage::listDatabases() const {
    std::vector<std::string> dbs;
    if (!std::filesystem::exists(data_dir_)) return dbs;
    for (const auto& entry : std::filesystem::directory_iterator(data_dir_)) {
        if (entry.is_directory()) {
            dbs.push_back(entry.path().filename().string());
        }
    }
    return dbs;
}

// ── Schema serialization ──────────────────────────────────────────────
//
//  Meta page payload (after the 16-byte common header):
//    root_page_id(4) | pk_index(4) | num_cols(4)
//    for each column: name_len(2) name type_len(2) type
//

std::string Storage::serializeSchema(const TableSchema& s) {
    std::string buf;

    // Placeholder for root_page_id — will be set later
    uint32_t root = INVALID_PAGE_ID;
    buf.append(reinterpret_cast<const char*>(&root), 4);

    uint32_t pk = static_cast<uint32_t>(s.primary_key_index);
    buf.append(reinterpret_cast<const char*>(&pk), 4);

    uint32_t nc = static_cast<uint32_t>(s.columns.size());
    buf.append(reinterpret_cast<const char*>(&nc), 4);

    for (const auto& col : s.columns) {
        uint16_t nl = static_cast<uint16_t>(col.name.size());
        buf.append(reinterpret_cast<const char*>(&nl), 2);
        buf.append(col.name);
        uint16_t tl = static_cast<uint16_t>(col.type.size());
        buf.append(reinterpret_cast<const char*>(&tl), 2);
        buf.append(col.type);
        uint16_t flags = 0;
        if (col.not_null) flags |= 0x0001;
        if (col.unique) flags |= 0x0002;
        if (col.has_default) flags |= 0x0004;
        if (!col.fk_ref_table.empty()) flags |= 0x0008;
        if (col.on_delete == OnDeleteAction::CASCADE) flags |= 0x0010;
        else if (col.on_delete == OnDeleteAction::SET_NULL) flags |= 0x0020;
        if (col.is_autoincrement) flags |= 0x0040;
        if (col.on_update == OnUpdateAction::CASCADE) flags |= 0x0080;
        else if (col.on_update == OnUpdateAction::SET_NULL) flags |= 0x0100;
        buf.append(reinterpret_cast<const char*>(&flags), 2);
        // Default value (if any)
        if (col.has_default) {
            uint16_t dl = static_cast<uint16_t>(col.default_value.size());
            buf.append(reinterpret_cast<const char*>(&dl), 2);
            buf.append(col.default_value);
        }
        // FK ref (if any)
        if (!col.fk_ref_table.empty()) {
            uint16_t trl = static_cast<uint16_t>(col.fk_ref_table.size());
            buf.append(reinterpret_cast<const char*>(&trl), 2);
            buf.append(col.fk_ref_table);
            uint16_t crl = static_cast<uint16_t>(col.fk_ref_column.size());
            buf.append(reinterpret_cast<const char*>(&crl), 2);
            buf.append(col.fk_ref_column);
        }
    }

    // Serialize secondary indexes
    uint32_t ni = static_cast<uint32_t>(s.indexes.size());
    buf.append(reinterpret_cast<const char*>(&ni), 4);
    for (const auto& idx : s.indexes) {
        uint16_t inl = static_cast<uint16_t>(idx.index_name.size());
        buf.append(reinterpret_cast<const char*>(&inl), 2);
        buf.append(idx.index_name);
        uint16_t icl = static_cast<uint16_t>(idx.column_name.size());
        buf.append(reinterpret_cast<const char*>(&icl), 2);
        buf.append(idx.column_name);
    }

    return buf;
}

TableSchema Storage::deserializeSchema(const char* data, uint32_t len) {
    TableSchema s;
    const char* p = data;
    const char* end = data + len;

    // skip root_page_id (read separately)
    p += 4;

    uint32_t pk; memcpy(&pk, p, 4); p += 4;
    s.primary_key_index = static_cast<int>(pk);

    uint32_t nc; memcpy(&nc, p, 4); p += 4;

    for (uint32_t i = 0; i < nc && p + 2 <= end; ++i) {
        ColumnDef cd;
        uint16_t nl; memcpy(&nl, p, 2); p += 2;
        if (p + nl > end) break;
        cd.name.assign(p, nl); p += nl;
        uint16_t tl; memcpy(&tl, p, 2); p += 2;
        if (p + tl > end) break;
        cd.type.assign(p, tl); p += tl;
        bool has_fk = false;
        if (p + 2 <= end) {
            uint16_t flags; memcpy(&flags, p, 2); p += 2;
            cd.not_null = (flags & 0x0001) != 0;
            cd.unique = (flags & 0x0002) != 0;
            cd.has_default = (flags & 0x0004) != 0;
            has_fk = (flags & 0x0008) != 0;
            if (flags & 0x0010) cd.on_delete = OnDeleteAction::CASCADE;
            else if (flags & 0x0020) cd.on_delete = OnDeleteAction::SET_NULL;
            else cd.on_delete = OnDeleteAction::NO_ACTION;
            cd.is_autoincrement = (flags & 0x0040) != 0;
            if (flags & 0x0080) cd.on_update = OnUpdateAction::CASCADE;
            else if (flags & 0x0100) cd.on_update = OnUpdateAction::SET_NULL;
            else cd.on_update = OnUpdateAction::NO_ACTION;
        }
        if (cd.has_default && p + 2 <= end) {
            uint16_t dl; memcpy(&dl, p, 2); p += 2;
            if (p + dl <= end) { cd.default_value.assign(p, dl); p += dl; }
        }
        if (has_fk && p + 2 <= end) {
            uint16_t trl; memcpy(&trl, p, 2); p += 2;
            if (p + trl <= end) { cd.fk_ref_table.assign(p, trl); p += trl; }
            if (p + 2 <= end) {
                uint16_t crl; memcpy(&crl, p, 2); p += 2;
                if (p + crl <= end) { cd.fk_ref_column.assign(p, crl); p += crl; }
            }
        }
        s.columns.push_back(std::move(cd));
    }

    // Deserialize secondary indexes (if data available)
    if (p + 4 <= end) {
        uint32_t ni; memcpy(&ni, p, 4); p += 4;
        for (uint32_t i = 0; i < ni && p + 2 <= end; ++i) {
            IndexDef idx;
            uint16_t inl; memcpy(&inl, p, 2); p += 2;
            if (p + inl > end) break;
            idx.index_name.assign(p, inl); p += inl;
            if (p + 2 > end) break;
            uint16_t icl; memcpy(&icl, p, 2); p += 2;
            if (p + icl > end) break;
            idx.column_name.assign(p, icl); p += icl;
            s.indexes.push_back(std::move(idx));
        }
    }

    return s;
}

// ── Table ops ──────────────────────────────────────────────────────────

std::vector<std::string> Storage::listTables(const std::string& db_name) const {
    std::vector<std::string> tables;
    auto p = dbPath(db_name);
    if (!std::filesystem::exists(p)) return tables;
    for (const auto& entry : std::filesystem::directory_iterator(p)) {
        if (entry.is_regular_file() && entry.path().extension() == ".db") {
            tables.push_back(entry.path().stem().string());
        }
    }
    return tables;
}

bool Storage::createTable(const std::string& db_name,
                          const TableSchema& schema) {
    if (!databaseExists(db_name)) return false;
    auto p = tablePath(db_name, schema.table_name);
    if (std::filesystem::exists(p)) return false;

    // Create the .db file with a meta page (page 0) and an empty root leaf (page 1)
    BufferPool& pool = getPool(p.string());

    // Page 0: Meta
    PageId meta_id;
    Page* meta = pool.newPage(&meta_id);  // should be 0
    meta->setPageType(META_PAGE);
    meta->setPageId(meta_id);

    // Page 1: Empty root leaf
    PageId root_id;
    Page* root = pool.newPage(&root_id);  // should be 1
    root->setPageType(LEAF_PAGE);
    root->setPageId(root_id);
    leafSetContentStart(*root, PAGE_SIZE);
    leafSetNextId(*root, INVALID_PAGE_ID);
    pool.unpinPage(root_id, true);

    // Write schema into meta page payload
    std::string payload = serializeSchema(schema);
    // Patch root_page_id at offset 0 of payload
    memcpy(&payload[0], &root_id, 4);

    if (16 + payload.size() > PAGE_SIZE)
        throw std::runtime_error("Schema too large for meta page");
    memcpy(meta->data + 16, payload.data(), payload.size());
    meta->setNumRecords(static_cast<uint32_t>(payload.size()));
    pool.unpinPage(meta_id, true);

    if (wal_mgr_) wal_mgr_->flushTo(wal_mgr_->getNextLSN() - 1);
    maybeCheckpoint();
    return true;
}

bool Storage::dropTable(const std::string& db_name,
                        const std::string& table_name) {
    auto p = tablePath(db_name, table_name);
    if (!std::filesystem::exists(p)) return false;
    closePool(p.string());

    // Also remove any .idx files for this table
    auto dir = dbPath(db_name);
    std::string prefix = table_name + ".";
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".idx") {
            std::string stem = entry.path().stem().string(); // e.g. "users.email"
            if (stem.substr(0, prefix.size()) == prefix) {
                closePool(entry.path().string());
                std::filesystem::remove(entry.path());
            }
        }
    }

    return std::filesystem::remove(p);
}

bool Storage::tableExists(const std::string& db_name,
                           const std::string& table_name) const {
    return std::filesystem::exists(tablePath(db_name, table_name));
}

TableSchema Storage::getTableSchema(const std::string& db_name,
                                     const std::string& table_name) const {
    auto p = tablePath(db_name, table_name);
    if (!std::filesystem::exists(p))
        throw std::runtime_error("Table '" + table_name + "' does not exist");

    BufferPool& pool = getPool(p.string());

    Page* meta = pool.fetchPage(0);
    TableSchema s = deserializeSchema(meta->data + 16, PAGE_SIZE - 16);
    s.table_name = table_name;
    pool.unpinPage(0, false);
    return s;
}

// ── Row operations ─────────────────────────────────────────────────────

std::vector<Row> Storage::readAllRows(const std::string& db_name,
                                       const std::string& table_name) const {
    auto p = tablePath(db_name, table_name);
    BufferPool& pool = getPool(p.string());

    // Read root_page_id and key type from meta
    Page* meta = pool.fetchPage(0);
    PageId root_id;
    memcpy(&root_id, meta->data + 16, 4);

    uint32_t payload_len = meta->getNumRecords();
    TableSchema s = deserializeSchema(meta->data + 16, payload_len);
    pool.unpinPage(0, false);

    BPlusTree tree(pool, root_id, &s, 1);
    return tree.scanAll();
}

int Storage::appendRows(const std::string& db_name,
                        const std::string& table_name,
                        const std::vector<Row>& rows) {
    auto p = tablePath(db_name, table_name);
    BufferPool& pool = getPool(p.string());

    // Read meta
    Page* meta = pool.fetchPage(0);
    PageId root_id;
    memcpy(&root_id, meta->data + 16, 4);

    uint32_t payload_len = meta->getNumRecords();
    TableSchema s = deserializeSchema(meta->data + 16, payload_len);
    pool.unpinPage(0, false);

    BPlusTree tree(pool, root_id, &s, 1);

    const std::string abs_table = p.string();

    // ROW_UPSERT in WAL before mutating pages (skip duplicates like plain insert).
    for (const auto& row : rows) {
        BTreeKey k = cluster_key_from_row(s, row);
        if (tree.search(k)) continue;
        std::string blob = serialize_row_disk(s, row);
        walAppendRowUpsert(abs_table, wal_encode_btree_key(k), blob);
    }
    walFlushDurably();

    int count = 0;
    for (const auto& row : rows) {
        if (tree.insert(cluster_key_from_row(s, row), row)) ++count;
    }

    // Update root in meta (may have changed due to splits)
    PageId new_root = tree.getRootPageId();
    meta = pool.fetchPage(0);
    memcpy(meta->data + 16, &new_root, 4);
    pool.unpinPage(0, true);

    walFlushDurably();
    maybeCheckpoint();
    return count;
}

bool Storage::writeAllRows(const std::string& db_name,
                           const std::string& table_name,
                           const std::vector<Row>& rows) {
    auto p = tablePath(db_name, table_name);

    // Read current schema from existing file
    TableSchema schema;
    {
        BufferPool& pool = getPool(p.string());
        Page* meta = pool.fetchPage(0);
        uint32_t payload_len = meta->getNumRecords();
        schema = deserializeSchema(meta->data + 16, payload_len);
        schema.table_name = table_name;
        pool.unpinPage(0, false);
    }
    return writeAllRows(db_name, table_name, rows, schema);
}

bool Storage::writeAllRows(const std::string& db_name,
                           const std::string& table_name,
                           const std::vector<Row>& rows,
                           const TableSchema& schema) {
    auto p = tablePath(db_name, table_name);

    // Delete and recreate the file
    closePool(p.string());
    std::filesystem::remove(p);

    int pk = schema.primary_key_index;
    BTreeBulkKeySpec bulk_spec;
    bulk_spec.col0 = pk;
    bulk_spec.col1 = -1;

    std::vector<Row> sorted = rows;
    std::sort(sorted.begin(), sorted.end(), [&](const Row& a, const Row& b) {
        return compare_btree_keys(BPlusTree::bulkExtractKey(a, bulk_spec),
                                  BPlusTree::bulkExtractKey(b, bulk_spec)) < 0;
    });

    // Create new file with meta page
    BufferPool& pool = getPool(p.string());

    PageId meta_id;
    Page* meta = pool.newPage(&meta_id);
    meta->setPageType(META_PAGE);
    meta->setPageId(meta_id);

    PageId root_id = BPlusTree::bulkLoad(pool, sorted, &schema, bulk_spec);

    // Write schema with correct root_id
    std::string payload = serializeSchema(schema);
    memcpy(&payload[0], &root_id, 4);
    memcpy(meta->data + 16, payload.data(), payload.size());
    meta->setNumRecords(static_cast<uint32_t>(payload.size()));
    pool.unpinPage(meta_id, true);

    if (wal_mgr_) wal_mgr_->flushTo(wal_mgr_->getNextLSN() - 1);
    maybeCheckpoint();
    return true;
}

// ── findRow ─────────────────────────────────────────────────────

Row Storage::findRow(const std::string& db_name,
                     const std::string& table_name,
                     const std::string& key) const {
    auto p = tablePath(db_name, table_name);
    BufferPool& pool = getPool(p.string());

    Page* meta = pool.fetchPage(0);
    PageId root_id;
    memcpy(&root_id, meta->data + 16, 4);
    uint32_t payload_len = meta->getNumRecords();
    TableSchema s = deserializeSchema(meta->data + 16, payload_len);
    pool.unpinPage(0, false);

    BPlusTree tree(pool, root_id, &s, 1);
    auto result = tree.search(cluster_key_from_literal(s, key));
    if (result.has_value()) return result.value();
    return Row{}; // empty = not found
}

// ── alterTableAddColumn ─────────────────────────────────────────

bool Storage::alterTableAddColumn(const std::string& db_name,
                                   const std::string& table_name,
                                   const ColumnDef& new_col) {
    if (!tableExists(db_name, table_name)) return false;

    // 1. Read current schema
    TableSchema schema = getTableSchema(db_name, table_name);

    // 2. Check column doesn't already exist
    for (const auto& col : schema.columns)
        if (col.name == new_col.name) return false;

    // 3. Read all existing rows
    auto rows = readAllRows(db_name, table_name);

    // 4. Add new column to schema
    schema.columns.push_back(new_col);

    // 5. Extend each row (legacy: empty string cell; typed: TEXT "" or NULL for non-TEXT)
    for (auto& row : rows) {
        if (new_col.has_default && !new_col.default_value.empty())
            row.push_back(coerce_string_to_cell_column(new_col, new_col.default_value, false));
        else if (new_col.type == "TEXT")
            row.push_back(CellPrimitive{std::string{}});
        else
            row.push_back(std::nullopt);
    }

    // 6. Rewrite file with new schema and extended rows
    return writeAllRows(db_name, table_name, rows, schema);
}

// ── alterTableDropColumn ────────────────────────────────────────

bool Storage::alterTableDropColumn(const std::string& db_name,
                                    const std::string& table_name,
                                    const std::string& col_name) {
    if (!tableExists(db_name, table_name)) return false;

    // 1. Read current schema
    TableSchema schema = getTableSchema(db_name, table_name);

    // 2. Find column index
    int drop_idx = -1;
    for (int i = 0; i < (int)schema.columns.size(); ++i)
        if (schema.columns[i].name == col_name) { drop_idx = i; break; }
    if (drop_idx < 0) return false;

    // 3. Cannot drop primary key column
    if (drop_idx == schema.primary_key_index) return false;

    // 4. Read all existing rows
    auto rows = readAllRows(db_name, table_name);

    // 5. Remove column from schema
    schema.columns.erase(schema.columns.begin() + drop_idx);
    // Adjust primary key index if needed
    if (drop_idx < schema.primary_key_index)
        schema.primary_key_index--;

    // 6. Remove field from each row
    for (auto& row : rows) {
        if (drop_idx < (int)row.size())
            row.erase(row.begin() + drop_idx);
    }

    // 7. Rewrite file with new schema and trimmed rows
    return writeAllRows(db_name, table_name, rows, schema);
}

// ── Secondary indexes ──────────────────────────────────────────────────

// Helper: rewrite meta page with updated schema (e.g. after adding/removing an index)
static void rewriteMeta(BufferPool& pool, WALManager* wal_mgr, const TableSchema& schema, PageId root_id) {
    Page* meta = pool.fetchPage(0);
    std::string payload = Storage::serializeSchemaPublic(schema);
    memcpy(&payload[0], &root_id, 4);
    memcpy(meta->data + 16, payload.data(), payload.size());
    meta->setNumRecords(static_cast<uint32_t>(payload.size()));
    pool.unpinPage(0, true);
    // No-Force: do not force dirty pages here. We only force the WAL.
    if (wal_mgr) wal_mgr->flushTo(wal_mgr->getNextLSN() - 1);
}

bool Storage::createIndex(const std::string& db_name, const std::string& table_name,
                           const std::string& index_name, const std::string& column_name) {
    if (!tableExists(db_name, table_name)) return false;

    TableSchema schema = getTableSchema(db_name, table_name);

    // Check column exists
    int col_idx = -1;
    for (int i = 0; i < (int)schema.columns.size(); ++i)
        if (schema.columns[i].name == column_name) { col_idx = i; break; }
    if (col_idx < 0) return false;

    // Check index doesn't already exist
    for (const auto& idx : schema.indexes)
        if (idx.index_name == index_name || idx.column_name == column_name) return false;

    // Create index file
    auto ip = indexPath(db_name, table_name, column_name);
    if (std::filesystem::exists(ip)) return false;

    // Read all rows
    auto rows = readAllRows(db_name, table_name);

    // Index leaf rows: [indexed column cell, PK cell] (typed B+-tree key = both cells)
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

    // Create the .idx file with meta page + bulk-loaded tree
    BufferPool& pool = getPool(ip.string());

    PageId meta_id;
    Page* meta = pool.newPage(&meta_id);
    meta->setPageType(META_PAGE);
    meta->setPageId(meta_id);

    PageId root_id = BPlusTree::bulkLoad(pool, idx_rows, &mini, ix_spec);

    // Store root_id in meta page
    memcpy(meta->data + 16, &root_id, 4);
    meta->setNumRecords(4);
    pool.unpinPage(meta_id, true);
    if (wal_mgr_) wal_mgr_->flushTo(wal_mgr_->getNextLSN() - 1);
    maybeCheckpoint();

    // Update table schema to include the new index
    schema.indexes.push_back({index_name, column_name});
    auto tp = tablePath(db_name, table_name);
    BufferPool& tpool = getPool(tp.string());
    Page* tmeta = tpool.fetchPage(0);
    PageId troot;
    memcpy(&troot, tmeta->data + 16, 4);
    tpool.unpinPage(0, false);
    rewriteMeta(tpool, wal_mgr_.get(), schema, troot);

    return true;
}

bool Storage::dropIndex(const std::string& db_name, const std::string& table_name,
                         const std::string& index_name) {
    if (!tableExists(db_name, table_name)) return false;

    TableSchema schema = getTableSchema(db_name, table_name);

    // Find the index
    int idx_pos = -1;
    for (int i = 0; i < (int)schema.indexes.size(); ++i)
        if (schema.indexes[i].index_name == index_name) { idx_pos = i; break; }
    if (idx_pos < 0) return false;

    std::string col_name = schema.indexes[idx_pos].column_name;

    // Remove index file
    auto ip = indexPath(db_name, table_name, col_name);
    if (std::filesystem::exists(ip)) {
        closePool(ip.string());
        std::filesystem::remove(ip);
    }

    // Update schema
    schema.indexes.erase(schema.indexes.begin() + idx_pos);
    auto tp = tablePath(db_name, table_name);
    BufferPool& tpool = getPool(tp.string());
    Page* tmeta = tpool.fetchPage(0);
    PageId troot;
    memcpy(&troot, tmeta->data + 16, 4);
    tpool.unpinPage(0, false);
    rewriteMeta(tpool, wal_mgr_.get(), schema, troot);

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
    PageId root_id; memcpy(&root_id, meta->data + 16, 4);
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

BufferPool& Storage::getPool(const std::string& path) const {
    if (pools_.find(path) == pools_.end()) {
        pools_[path] = std::make_unique<BufferPool>(path, POOL_SIZE, wal_mgr_.get());
    }
    return *pools_[path];
}

void Storage::closePool(const std::string& path) const {
    pools_.erase(path);
}

} // namespace db
