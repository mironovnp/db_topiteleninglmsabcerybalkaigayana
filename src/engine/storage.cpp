#include "engine/storage.hpp"
#include "engine/buffer_pool.hpp"
#include "engine/btree.hpp"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <stdexcept>

namespace db {

Storage::Storage(const std::string& data_dir) : data_dir_(data_dir) {
    std::filesystem::create_directories(data_dir_);
}

std::filesystem::path Storage::dbPath(const std::string& db) const {
    return data_dir_ / db;
}

std::filesystem::path Storage::tablePath(const std::string& db,
                                          const std::string& tbl) const {
    return data_dir_ / db / (tbl + ".db");
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
        // Constraint flags: 1 byte
        uint8_t flags = 0;
        if (col.not_null) flags |= 0x01;
        if (col.unique) flags |= 0x02;
        if (col.has_default) flags |= 0x04;
        if (!col.fk_ref_table.empty()) flags |= 0x08;
        if (col.on_delete == OnDeleteAction::CASCADE) flags |= 0x10;
        else if (col.on_delete == OnDeleteAction::SET_NULL) flags |= 0x20;
        buf.push_back(static_cast<char>(flags));
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
        // Constraint flags (if data available)
        if (p < end) {
            uint8_t flags = static_cast<uint8_t>(*p); p++;
            cd.not_null = (flags & 0x01) != 0;
            cd.unique = (flags & 0x02) != 0;
            cd.has_default = (flags & 0x04) != 0;
            bool has_fk = (flags & 0x08) != 0;
            if ((flags & 0x10) != 0) cd.on_delete = OnDeleteAction::CASCADE;
            else if ((flags & 0x20) != 0) cd.on_delete = OnDeleteAction::SET_NULL;
            else cd.on_delete = OnDeleteAction::NO_ACTION;
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
        }
        s.columns.push_back(std::move(cd));
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
    BufferPool pool(p.string());

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

    pool.flushAll();
    return true;
}

bool Storage::dropTable(const std::string& db_name,
                        const std::string& table_name) {
    auto p = tablePath(db_name, table_name);
    if (!std::filesystem::exists(p)) return false;
    return std::filesystem::remove(p);
}

bool Storage::tableExists(const std::string& db_name,
                           const std::string& table_name) const {
    return std::filesystem::exists(tablePath(db_name, table_name));
}

TableSchema Storage::getTableSchema(const std::string& db_name,
                                     const std::string& table_name) const {
    auto p = tablePath(db_name, table_name);
    BufferPool pool(p.string());

    Page* meta = pool.fetchPage(0);
    uint32_t payload_len = meta->getNumRecords();  // we stored payload size here
    TableSchema s = deserializeSchema(meta->data + 16, payload_len);
    s.table_name = table_name;
    pool.unpinPage(0, false);
    return s;
}

// ── Row operations ─────────────────────────────────────────────────────

std::vector<Row> Storage::readAllRows(const std::string& db_name,
                                       const std::string& table_name) const {
    auto p = tablePath(db_name, table_name);
    BufferPool pool(p.string());

    // Read root_page_id and key type from meta
    Page* meta = pool.fetchPage(0);
    PageId root_id;
    memcpy(&root_id, meta->data + 16, 4);

    uint32_t payload_len = meta->getNumRecords();
    TableSchema s = deserializeSchema(meta->data + 16, payload_len);
    pool.unpinPage(0, false);

    std::string key_type = s.columns.empty() ? "TEXT"
                           : s.columns[s.primary_key_index].type;

    BPlusTree tree(pool, root_id, key_type);
    return tree.scanAll();
}

int Storage::appendRows(const std::string& db_name,
                        const std::string& table_name,
                        const std::vector<Row>& rows) {
    auto p = tablePath(db_name, table_name);
    BufferPool pool(p.string());

    // Read meta
    Page* meta = pool.fetchPage(0);
    PageId root_id;
    memcpy(&root_id, meta->data + 16, 4);

    uint32_t payload_len = meta->getNumRecords();
    TableSchema s = deserializeSchema(meta->data + 16, payload_len);
    pool.unpinPage(0, false);

    std::string key_type = s.columns.empty() ? "TEXT"
                           : s.columns[s.primary_key_index].type;

    BPlusTree tree(pool, root_id, key_type);

    int count = 0;
    for (const auto& row : rows) {
        std::string key = row[s.primary_key_index];
        if (tree.insert(key, row)) ++count;
    }

    // Update root in meta (may have changed due to splits)
    PageId new_root = tree.getRootPageId();
    meta = pool.fetchPage(0);
    memcpy(meta->data + 16, &new_root, 4);
    pool.unpinPage(0, true);

    pool.flushAll();
    return count;
}

bool Storage::writeAllRows(const std::string& db_name,
                           const std::string& table_name,
                           const std::vector<Row>& rows) {
    auto p = tablePath(db_name, table_name);

    // Read current schema from existing file
    TableSchema schema;
    {
        BufferPool pool(p.string());
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
    std::filesystem::remove(p);

    // Sort rows by primary key
    std::string key_type = schema.columns.empty() ? "TEXT"
                           : schema.columns[schema.primary_key_index].type;
    int pk = schema.primary_key_index;

    std::vector<Row> sorted = rows;
    std::sort(sorted.begin(), sorted.end(),
              [&](const Row& a, const Row& b) {
                  if (key_type == "INT") {
                      long la = 0, lb = 0;
                      try { la = std::stol(a[pk]); } catch (...) {}
                      try { lb = std::stol(b[pk]); } catch (...) {}
                      return la < lb;
                  }
                  if (key_type == "FLOAT") {
                      double da = 0, db2 = 0;
                      try { da = std::stod(a[pk]); } catch (...) {}
                      try { db2 = std::stod(b[pk]); } catch (...) {}
                      return da < db2;
                  }
                  return a[pk] < b[pk];
              });

    // Create new file with meta page
    BufferPool pool(p.string());

    PageId meta_id;
    Page* meta = pool.newPage(&meta_id);
    meta->setPageType(META_PAGE);
    meta->setPageId(meta_id);

    // Bulk load the B+ tree
    PageId root_id = BPlusTree::bulkLoad(pool, sorted,
                                          static_cast<uint32_t>(pk),
                                          key_type);

    // Write schema with correct root_id
    std::string payload = serializeSchema(schema);
    memcpy(&payload[0], &root_id, 4);
    memcpy(meta->data + 16, payload.data(), payload.size());
    meta->setNumRecords(static_cast<uint32_t>(payload.size()));
    pool.unpinPage(meta_id, true);

    pool.flushAll();
    return true;
}

// ── findRow ─────────────────────────────────────────────────────

Row Storage::findRow(const std::string& db_name,
                     const std::string& table_name,
                     const std::string& key) const {
    auto p = tablePath(db_name, table_name);
    BufferPool pool(p.string());

    Page* meta = pool.fetchPage(0);
    PageId root_id;
    memcpy(&root_id, meta->data + 16, 4);
    uint32_t payload_len = meta->getNumRecords();
    TableSchema s = deserializeSchema(meta->data + 16, payload_len);
    pool.unpinPage(0, false);

    std::string key_type = s.columns.empty() ? "TEXT"
                           : s.columns[s.primary_key_index].type;

    BPlusTree tree(pool, root_id, key_type);
    auto result = tree.search(key);
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

    // 5. Extend each row with default empty string
    for (auto& row : rows)
        row.push_back("");

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

} // namespace db
