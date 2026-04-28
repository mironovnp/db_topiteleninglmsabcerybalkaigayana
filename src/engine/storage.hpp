#pragma once
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
};

using Row = std::vector<std::string>;

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
    int appendRows(const std::string& db_name, const std::string& table_name,
                   const std::vector<Row>& rows);

private:
    std::filesystem::path data_dir_;
    std::filesystem::path dbPath(const std::string& db) const;
    std::filesystem::path tablePath(const std::string& db, const std::string& tbl) const;
    static std::string escapeCSV(const std::string& f);
    static std::vector<std::string> parseCSVLine(const std::string& line);
    static std::string rowToCSV(const Row& row);
};

} // namespace db
