#include "engine/storage/storage.hpp"
#include "engine/buffer_pool.hpp"
#include "engine/btree.hpp"
#include "engine/row_codec.hpp"
#include "engine/storage/storage_internal.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <stdexcept>

namespace db {

using storage_i::cluster_key_from_literal;
using storage_i::cluster_key_from_row;
using storage_i::wal_encode_btree_key;

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

bool Storage::createTable(const std::string& db_name, const TableSchema& schema) {
    if (!databaseExists(db_name)) return false;
    auto p = tablePath(db_name, schema.table_name);
    if (std::filesystem::exists(p)) return false;

    BufferPool& pool = getPool(p.string());

    PageId meta_id;
    Page* meta = pool.newPage(&meta_id);
    meta->setPageType(META_PAGE);
    meta->setPageId(meta_id);

    PageId root_id;
    Page* root = pool.newPage(&root_id);
    root->setPageType(LEAF_PAGE);
    root->setPageId(root_id);
    leafSetContentStart(*root, PAGE_SIZE);
    leafSetNextId(*root, INVALID_PAGE_ID);
    walLogPageImage(p.string(), root_id, *root);
    pool.unpinPage(root_id, true);

    std::string payload = serializeSchema(schema);
    memcpy(&payload[0], &root_id, 4);

    if (16 + payload.size() > PAGE_SIZE)
        throw std::runtime_error("Schema too large for meta page");
    memcpy(meta->data + 16, payload.data(), payload.size());
    meta->setNumRecords(static_cast<uint32_t>(payload.size()));

    walLogPageImage(p.string(), meta_id, *meta);
    pool.unpinPage(meta_id, true);

    if (wal_mgr_) wal_mgr_->flushTo(wal_mgr_->getNextLSN() - 1);
    maybeCheckpoint();
    return true;
}

bool Storage::dropTable(const std::string& db_name, const std::string& table_name) {
    auto p = tablePath(db_name, table_name);
    if (!std::filesystem::exists(p)) return false;
    closePool(p.string());

    auto dir = dbPath(db_name);
    std::string prefix = table_name + ".";
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".idx") {
            std::string stem = entry.path().stem().string();
            if (stem.substr(0, prefix.size()) == prefix) {
                closePool(entry.path().string());
                std::filesystem::remove(entry.path());
            }
        }
    }

    return std::filesystem::remove(p);
}

bool Storage::tableExists(const std::string& db_name, const std::string& table_name) const {
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

std::vector<Row> Storage::readAllRows(const std::string& db_name,
                                       const std::string& table_name) const {
    auto p = tablePath(db_name, table_name);
    BufferPool& pool = getPool(p.string());

    Page* meta = pool.fetchPage(0);
    PageId root_id;
    memcpy(&root_id, meta->data + 16, 4);
    if (root_id == 0 || root_id == INVALID_PAGE_ID) {
        pool.unpinPage(0, false);
        throw std::runtime_error("Table metadata corrupted: root_id is invalid (0 or -1)");
    }

    uint32_t payload_len = meta->getNumRecords();
    TableSchema s = deserializeSchema(meta->data + 16, payload_len);
    pool.unpinPage(0, false);

    BPlusTree tree(pool, root_id, &s, 1);
    return tree.scanAll();
}

int Storage::appendRows(const std::string& db_name, const std::string& table_name,
                        const std::vector<Row>& rows) {
    auto p = tablePath(db_name, table_name);
    BufferPool& pool = getPool(p.string());

    Page* meta = pool.fetchPage(0);
    PageId root_id;
    memcpy(&root_id, meta->data + 16, 4);
    if (root_id == 0 || root_id == INVALID_PAGE_ID) {
        pool.unpinPage(0, false);
        throw std::runtime_error("Table metadata corrupted: root_id is invalid (0 or -1) in appendRows");
    }

    uint32_t payload_len = meta->getNumRecords();
    TableSchema s = deserializeSchema(meta->data + 16, payload_len);
    pool.unpinPage(0, false);

    BPlusTree tree(pool, root_id, &s, 1);

    const std::string abs_table = p.string();

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

    PageId new_root = tree.getRootPageId();
    meta = pool.fetchPage(0);
    memcpy(meta->data + 16, &new_root, 4);
    walLogPageImage(abs_table, 0, *meta);
    pool.unpinPage(0, true);

    walFlushDurably();
    maybeCheckpoint();
    return count;
}

bool Storage::writeAllRows(const std::string& db_name, const std::string& table_name,
                           const std::vector<Row>& rows) {
    auto p = tablePath(db_name, table_name);

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

bool Storage::writeAllRows(const std::string& db_name, const std::string& table_name,
                           const std::vector<Row>& rows, const TableSchema& schema) {
    auto p = tablePath(db_name, table_name);

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

    BufferPool& pool = getPool(p.string());

    PageId meta_id;
    Page* meta = pool.newPage(&meta_id);
    meta->setPageType(META_PAGE);
    meta->setPageId(meta_id);

    PageId root_id = BPlusTree::bulkLoad(pool, sorted, &schema, bulk_spec);

    std::string payload = serializeSchema(schema);
    memcpy(&payload[0], &root_id, 4);
    memcpy(meta->data + 16, payload.data(), payload.size());
    meta->setNumRecords(static_cast<uint32_t>(payload.size()));
    walLogPageImage(p.string(), meta_id, *meta);
    pool.unpinPage(meta_id, true);

    if (wal_mgr_) wal_mgr_->flushTo(wal_mgr_->getNextLSN() - 1);
    maybeCheckpoint();
    return true;
}

Row Storage::findRow(const std::string& db_name, const std::string& table_name,
                     const std::string& key) const {
    auto p = tablePath(db_name, table_name);
    BufferPool& pool = getPool(p.string());

    Page* meta = pool.fetchPage(0);
    PageId root_id;
    memcpy(&root_id, meta->data + 16, 4);
    if (root_id == 0 || root_id == INVALID_PAGE_ID) {
        pool.unpinPage(0, false);
        throw std::runtime_error("Table metadata corrupted: root_id is invalid (0 or -1) in findRow");
    }
    uint32_t payload_len = meta->getNumRecords();
    TableSchema s = deserializeSchema(meta->data + 16, payload_len);
    pool.unpinPage(0, false);

    BPlusTree tree(pool, root_id, &s, 1);
    auto result = tree.search(cluster_key_from_literal(s, key));
    if (result.has_value()) return result.value();
    return Row{};
}

bool Storage::alterTableAddColumn(const std::string& db_name, const std::string& table_name,
                                   const ColumnDef& new_col) {
    if (!tableExists(db_name, table_name)) return false;

    TableSchema schema = getTableSchema(db_name, table_name);

    for (const auto& col : schema.columns)
        if (col.name == new_col.name) return false;

    auto rows = readAllRows(db_name, table_name);

    schema.columns.push_back(new_col);

    for (auto& row : rows) {
        if (new_col.has_default && !new_col.default_value.empty())
            row.push_back(coerce_string_to_cell_column(new_col, new_col.default_value, false));
        else if (new_col.type == "TEXT")
            row.push_back(CellPrimitive{std::string{}});
        else
            row.push_back(std::nullopt);
    }

    return writeAllRows(db_name, table_name, rows, schema);
}

bool Storage::alterTableDropColumn(const std::string& db_name, const std::string& table_name,
                                    const std::string& col_name) {
    if (!tableExists(db_name, table_name)) return false;

    TableSchema schema = getTableSchema(db_name, table_name);

    int drop_idx = -1;
    for (int i = 0; i < (int)schema.columns.size(); ++i)
        if (schema.columns[i].name == col_name) {
            drop_idx = i;
            break;
        }
    if (drop_idx < 0) return false;

    if (drop_idx == schema.primary_key_index) return false;

    auto rows = readAllRows(db_name, table_name);

    schema.columns.erase(schema.columns.begin() + drop_idx);
    if (drop_idx < schema.primary_key_index)
        schema.primary_key_index--;

    for (auto& row : rows) {
        if (drop_idx < (int)row.size())
            row.erase(row.begin() + drop_idx);
    }

    return writeAllRows(db_name, table_name, rows, schema);
}

} // namespace db
