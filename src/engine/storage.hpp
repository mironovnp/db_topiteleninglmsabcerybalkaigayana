#pragma once
#include "engine/page.hpp"      // Row lives here now
#include "engine/wal.hpp"
#include "engine/buffer_pool.hpp"
#include <string>
#include <vector>
#include <filesystem>
#include <memory>
#include <unordered_map>

namespace db {

enum class OnDeleteAction {
    NO_ACTION = 0,
    CASCADE = 1,
    SET_NULL = 2
};

enum class OnUpdateAction {
    NO_ACTION = 0,
    CASCADE = 1,
    SET_NULL = 2
};

struct ColumnDef {
    std::string name;
    std::string type; // INT, FLOAT, BOOL, TEXT, VARCHAR(N)
    bool not_null = false;
    bool unique = false;
    bool has_default = false;
    bool is_autoincrement = false;
    std::string default_value;
    std::string fk_ref_table;
    std::string fk_ref_column;
    OnDeleteAction on_delete = OnDeleteAction::NO_ACTION;
    OnUpdateAction on_update = OnUpdateAction::NO_ACTION;
};

struct IndexDef {
    std::string index_name;
    std::string column_name;
};

struct TableSchema {
    std::string table_name;
    std::vector<ColumnDef> columns;
    int primary_key_index = 0;      // default: first column
    std::vector<IndexDef> indexes;  // secondary indexes
};

class Storage {
public:
    explicit Storage(const std::string& data_dir = "data");

    // Database
    bool createDatabase(const std::string& db_name);
    bool dropDatabase(const std::string& db_name);
    bool databaseExists(const std::string& db_name) const;
    std::vector<std::string> listDatabases() const;

    // Table
    bool createTable(const std::string& db_name, const TableSchema& schema);
    bool dropTable(const std::string& db_name, const std::string& table_name);
    bool tableExists(const std::string& db_name, const std::string& table_name) const;
    TableSchema getTableSchema(const std::string& db_name, const std::string& table_name) const;
    std::vector<std::string> listTables(const std::string& db_name) const;

    // Rows
    std::vector<Row> readAllRows(const std::string& db_name, const std::string& table_name) const;
    bool writeAllRows(const std::string& db_name, const std::string& table_name,
                      const std::vector<Row>& rows);
    // writeAllRows with explicit schema (used by alterTableAddColumn)
    bool writeAllRows(const std::string& db_name, const std::string& table_name,
                      const std::vector<Row>& rows, const TableSchema& schema);
    int appendRows(const std::string& db_name, const std::string& table_name,
                   const std::vector<Row>& rows);
    // B+ tree point lookup — returns empty Row if key not found
    Row findRow(const std::string& db_name, const std::string& table_name,
                const std::string& key) const;

    // DDL mutation
    bool alterTableAddColumn(const std::string& db_name,
                             const std::string& table_name,
                             const ColumnDef& new_col);
    bool alterTableDropColumn(const std::string& db_name,
                              const std::string& table_name,
                              const std::string& col_name);

    // Secondary indexes
    bool createIndex(const std::string& db_name, const std::string& table_name,
                     const std::string& index_name, const std::string& column_name);
    bool dropIndex(const std::string& db_name, const std::string& table_name,
                   const std::string& index_name);
    // Lookup via secondary index: returns list of primary keys matching the value
    std::vector<std::string> indexLookup(const std::string& db_name,
                                         const std::string& table_name,
                                         const std::string& column_name,
                                         const std::string& value) const;
    std::vector<std::string> indexScan(const std::string& db_name,
                                       const std::string& table_name,
                                       const std::string& column_name,
                                       const std::string* low,
                                       const std::string* high) const;
    // Insert/remove entries from all secondary indexes of a table
    void indexInsertRow(const std::string& db_name, const std::string& table_name,
                        const TableSchema& schema, const Row& row);
    void indexRemoveRow(const std::string& db_name, const std::string& table_name,
                        const TableSchema& schema, const Row& row);
    // Check if a secondary index exists for a column
    bool hasIndex(const std::string& db_name, const std::string& table_name,
                  const std::string& column_name) const;

    // Public access to schema serialization (used by index helpers)
    static std::string serializeSchemaPublic(const TableSchema& s) { return serializeSchema(s); }

private:
    std::filesystem::path data_dir_;
    std::unique_ptr<WALManager> wal_mgr_;
    mutable std::unordered_map<std::string, std::unique_ptr<BufferPool>> pools_;

    BufferPool& getPool(const std::string& path) const;
    void closePool(const std::string& path) const;
    void flushAllPools() const;
    void maybeCheckpoint();

    std::filesystem::path dbPath(const std::string& db) const;
    std::filesystem::path tablePath(const std::string& db, const std::string& tbl) const;
    std::filesystem::path indexPath(const std::string& db, const std::string& tbl,
                                    const std::string& col) const;

    // Meta page serialization
    static std::string serializeSchema(const TableSchema& s);
    static TableSchema deserializeSchema(const char* data, uint32_t len);
};

} // namespace db
