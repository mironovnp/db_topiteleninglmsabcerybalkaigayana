#pragma once
#include "engine/page.hpp"      // Row lives here now
#include <string>
#include <vector>
#include <filesystem>

namespace db {

struct ColumnDef {
    std::string name;
    std::string type; // INT, FLOAT, BOOL, TEXT, VARCHAR(N)
};

struct TableSchema {
    std::string table_name;
    std::vector<ColumnDef> columns;
    int primary_key_index = 0;      // default: first column
};

class Storage {
public:
    explicit Storage(const std::string& data_dir = "data");

    // Database
    bool createDatabase(const std::string& db_name);
    bool dropDatabase(const std::string& db_name);
    bool databaseExists(const std::string& db_name) const;

    // Table
    bool createTable(const std::string& db_name, const TableSchema& schema);
    bool dropTable(const std::string& db_name, const std::string& table_name);
    bool tableExists(const std::string& db_name, const std::string& table_name) const;
    TableSchema getTableSchema(const std::string& db_name, const std::string& table_name) const;

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

private:
    std::filesystem::path data_dir_;
    std::filesystem::path dbPath(const std::string& db) const;
    std::filesystem::path tablePath(const std::string& db, const std::string& tbl) const;

    // Meta page serialization
    static std::string serializeSchema(const TableSchema& s);
    static TableSchema deserializeSchema(const char* data, uint32_t len);
};

} // namespace db
