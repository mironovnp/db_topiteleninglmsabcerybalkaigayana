#include "engine/storage.hpp"
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

std::filesystem::path Storage::tablePath(const std::string& db, const std::string& tbl) const {
    return data_dir_ / db / (tbl + ".csv");
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

// ── Table ops ──────────────────────────────────────────────────────────

bool Storage::createTable(const std::string& db_name, const TableSchema& schema) {
    if (!databaseExists(db_name)) return false;
    auto p = tablePath(db_name, schema.table_name);
    if (std::filesystem::exists(p)) return false;

    std::ofstream f(p);
    if (!f.is_open()) return false;

    for (size_t i = 0; i < schema.columns.size(); ++i) {
        if (i > 0) f << ",";
        f << schema.columns[i].name << ":" << schema.columns[i].type;
    }
    f << "\n";
    return true;
}

bool Storage::dropTable(const std::string& db_name, const std::string& table_name) {
    auto p = tablePath(db_name, table_name);
    if (!std::filesystem::exists(p)) return false;
    return std::filesystem::remove(p);
}

bool Storage::tableExists(const std::string& db_name, const std::string& table_name) const {
    return std::filesystem::exists(tablePath(db_name, table_name));
}

TableSchema Storage::getTableSchema(const std::string& db_name, const std::string& table_name) const {
    auto p = tablePath(db_name, table_name);
    std::ifstream f(p);
    if (!f.is_open()) throw std::runtime_error("Cannot open table: " + table_name);

    std::string header;
    std::getline(f, header);
    if (!header.empty() && header.back() == '\r') header.pop_back();

    TableSchema schema;
    schema.table_name = table_name;

    for (const auto& field : parseCSVLine(header)) {
        auto pos = field.find(':');
        if (pos == std::string::npos)
            throw std::runtime_error("Invalid schema in table: " + table_name);
        schema.columns.push_back({field.substr(0, pos), field.substr(pos + 1)});
    }
    return schema;
}

// ── CSV helpers ────────────────────────────────────────────────────────

std::string Storage::escapeCSV(const std::string& f) {
    bool need = false;
    for (char c : f)
        if (c == ',' || c == '"' || c == '\n') { need = true; break; }
    if (!need) return f;

    std::string r = "\"";
    for (char c : f) {
        if (c == '"') r += "\"\"";
        else r += c;
    }
    r += "\"";
    return r;
}

std::vector<std::string> Storage::parseCSVLine(const std::string& line) {
    std::vector<std::string> fields;
    std::string cur;
    bool inQ = false;

    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (inQ) {
            if (c == '"') {
                if (i + 1 < line.size() && line[i + 1] == '"') { cur += '"'; ++i; }
                else inQ = false;
            } else cur += c;
        } else {
            if (c == '"') inQ = true;
            else if (c == ',') { fields.push_back(cur); cur.clear(); }
            else cur += c;
        }
    }
    fields.push_back(cur);
    return fields;
}

std::string Storage::rowToCSV(const Row& row) {
    std::string r;
    for (size_t i = 0; i < row.size(); ++i) {
        if (i > 0) r += ",";
        r += escapeCSV(row[i]);
    }
    return r;
}

// ── Row ops ────────────────────────────────────────────────────────────

std::vector<Row> Storage::readAllRows(const std::string& db_name,
                                       const std::string& table_name) const {
    auto p = tablePath(db_name, table_name);
    std::ifstream f(p);
    if (!f.is_open()) throw std::runtime_error("Cannot open table: " + table_name);

    std::vector<Row> rows;
    std::string line;
    std::getline(f, line); // skip header

    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        rows.push_back(parseCSVLine(line));
    }
    return rows;
}

bool Storage::writeAllRows(const std::string& db_name, const std::string& table_name,
                            const std::vector<Row>& rows) {
    auto p = tablePath(db_name, table_name);
    std::string header;
    {
        std::ifstream f(p);
        if (!f.is_open()) return false;
        std::getline(f, header);
    }

    std::ofstream f(p, std::ios::trunc);
    if (!f.is_open()) return false;
    f << header << "\n";
    for (const auto& row : rows) f << rowToCSV(row) << "\n";
    return true;
}

int Storage::appendRows(const std::string& db_name, const std::string& table_name,
                         const std::vector<Row>& rows) {
    auto p = tablePath(db_name, table_name);
    std::ofstream f(p, std::ios::app);
    if (!f.is_open()) return 0;

    int n = 0;
    for (const auto& row : rows) { f << rowToCSV(row) << "\n"; ++n; }
    return n;
}

} // namespace db
