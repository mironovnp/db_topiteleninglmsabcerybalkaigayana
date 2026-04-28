#include "engine/executor.hpp"
#include <stdexcept>
#include <algorithm>

namespace db {

using json = nlohmann::json;

Executor::Executor(const std::string& data_dir) : storage_(data_dir) {}

json Executor::ok(const std::string& msg) {
    return {{"success", true}, {"message", msg}, {"type", "ddl"}};
}

json Executor::err(const std::string& msg) {
    return {{"success", false}, {"message", msg}};
}

void Executor::requireDB() const {
    if (current_db_.empty())
        throw std::runtime_error("No database selected. Use: USE <database>;");
}

int Executor::colIndex(const TableSchema& s, const std::string& name) const {
    for (size_t i = 0; i < s.columns.size(); ++i)
        if (s.columns[i].name == name) return static_cast<int>(i);
    return -1;
}

int Executor::compareValues(const std::string& a, const std::string& b,
                             const std::string& type) const {
    if (type == "INT") {
        long la = std::stol(a), lb = std::stol(b);
        return (la < lb) ? -1 : (la > lb) ? 1 : 0;
    }
    if (type == "FLOAT") {
        double da = std::stod(a), db = std::stod(b);
        return (da < db) ? -1 : (da > db) ? 1 : 0;
    }
    return a.compare(b);
}

bool Executor::evalWhere(const WhereExpr& expr, const Row& row,
                          const TableSchema& schema) const {
    switch (expr.kind) {
    case WhereExpr::CMP: {
        int idx = colIndex(schema, expr.column);
        if (idx < 0) throw std::runtime_error("Unknown column: " + expr.column);
        const std::string& val = row[idx];
        const std::string& type = schema.columns[idx].type;
        int cmp = compareValues(val, expr.value, type);
        if (expr.op == "=")  return cmp == 0;
        if (expr.op == "!=") return cmp != 0;
        if (expr.op == "<")  return cmp < 0;
        if (expr.op == ">")  return cmp > 0;
        if (expr.op == "<=") return cmp <= 0;
        if (expr.op == ">=") return cmp >= 0;
        return false;
    }
    case WhereExpr::AND_OP:
        return evalWhere(*expr.left, row, schema) &&
               evalWhere(*expr.right, row, schema);
    case WhereExpr::OR_OP:
        return evalWhere(*expr.left, row, schema) ||
               evalWhere(*expr.right, row, schema);
    case WhereExpr::NOT_OP:
        return !evalWhere(*expr.left, row, schema);
    }
    return false;
}

// ── Main entry point ───────────────────────────────────────────────────

json Executor::execute(const std::string& sql) {
    try {
        Lexer lexer(sql);
        auto tokens = lexer.tokenize();
        Parser parser(tokens);
        auto query = parser.parse();

        switch (query.type) {
        case QueryType::CREATE_DATABASE: return execCreateDB(query);
        case QueryType::DROP_DATABASE:   return execDropDB(query);
        case QueryType::CREATE_TABLE:    return execCreateTable(query);
        case QueryType::DROP_TABLE:      return execDropTable(query);
        case QueryType::SELECT:          return execSelect(query);
        case QueryType::INSERT:          return execInsert(query);
        case QueryType::UPDATE:          return execUpdate(query);
        case QueryType::DELETE_Q:        return execDelete(query);
        case QueryType::USE_DATABASE:    return execUse(query);
        }
        return err("Unknown query type");
    } catch (const std::exception& e) {
        return err(e.what());
    }
}

// ── DDL ────────────────────────────────────────────────────────────────

json Executor::execCreateDB(const ParsedQuery& q) {
    if (storage_.createDatabase(q.database_name))
        return ok("Database '" + q.database_name + "' created.");
    return err("Database '" + q.database_name + "' already exists.");
}

json Executor::execDropDB(const ParsedQuery& q) {
    if (storage_.dropDatabase(q.database_name)) {
        if (current_db_ == q.database_name) current_db_.clear();
        return ok("Database '" + q.database_name + "' dropped.");
    }
    return err("Database '" + q.database_name + "' does not exist.");
}

json Executor::execUse(const ParsedQuery& q) {
    if (!storage_.databaseExists(q.database_name))
        return err("Database '" + q.database_name + "' does not exist.");
    current_db_ = q.database_name;
    return ok("Using database '" + q.database_name + "'.");
}

json Executor::execCreateTable(const ParsedQuery& q) {
    requireDB();
    TableSchema schema;
    schema.table_name = q.table_name;
    for (const auto& cd : q.column_defs)
        schema.columns.push_back({cd.name, cd.type});

    if (storage_.createTable(current_db_, schema))
        return ok("Table '" + q.table_name + "' created.");
    return err("Table '" + q.table_name + "' already exists or DB not found.");
}

json Executor::execDropTable(const ParsedQuery& q) {
    requireDB();
    if (storage_.dropTable(current_db_, q.table_name))
        return ok("Table '" + q.table_name + "' dropped.");
    return err("Table '" + q.table_name + "' does not exist.");
}

// ── SELECT ─────────────────────────────────────────────────────────────

json Executor::execSelect(const ParsedQuery& q) {
    requireDB();
    auto schema = storage_.getTableSchema(current_db_, q.table_name);
    auto rows = storage_.readAllRows(current_db_, q.table_name);

    // Filter by WHERE
    if (q.where) {
        std::vector<Row> filtered;
        for (const auto& row : rows)
            if (evalWhere(*q.where, row, schema))
                filtered.push_back(row);
        rows = std::move(filtered);
    }

    // Determine columns to return
    std::vector<int> indices;
    std::vector<std::string> colNames;
    if (q.select_all) {
        for (size_t i = 0; i < schema.columns.size(); ++i) {
            indices.push_back(static_cast<int>(i));
            colNames.push_back(schema.columns[i].name);
        }
    } else {
        for (const auto& col : q.select_columns) {
            int idx = colIndex(schema, col);
            if (idx < 0) return err("Unknown column: " + col);
            indices.push_back(idx);
            colNames.push_back(col);
        }
    }

    // Build result
    json result;
    result["success"] = true;
    result["type"] = "select";
    result["columns"] = colNames;
    result["rows"] = json::array();

    for (const auto& row : rows) {
        json jr = json::array();
        for (int idx : indices) jr.push_back(row[idx]);
        result["rows"].push_back(jr);
    }
    result["message"] = std::to_string(rows.size()) + " row(s) returned.";
    return result;
}

// ── INSERT ─────────────────────────────────────────────────────────────

json Executor::execInsert(const ParsedQuery& q) {
    requireDB();
    auto schema = storage_.getTableSchema(current_db_, q.table_name);

    std::vector<Row> rows;
    for (const auto& vals : q.insert_values) {
        if (!q.insert_columns.empty()) {
            // Map named columns to row positions
            Row row(schema.columns.size(), "");
            for (size_t i = 0; i < q.insert_columns.size(); ++i) {
                int idx = colIndex(schema, q.insert_columns[i]);
                if (idx < 0) return err("Unknown column: " + q.insert_columns[i]);
                row[idx] = vals[i];
            }
            rows.push_back(std::move(row));
        } else {
            if (vals.size() != schema.columns.size())
                return err("Column count mismatch: expected " +
                           std::to_string(schema.columns.size()) +
                           ", got " + std::to_string(vals.size()));
            rows.push_back(vals);
        }
    }

    int n = storage_.appendRows(current_db_, q.table_name, rows);
    json result;
    result["success"] = true;
    result["type"] = "modify";
    result["affected_rows"] = n;
    result["message"] = std::to_string(n) + " row(s) inserted.";
    return result;
}

// ── UPDATE ─────────────────────────────────────────────────────────────

json Executor::execUpdate(const ParsedQuery& q) {
    requireDB();
    auto schema = storage_.getTableSchema(current_db_, q.table_name);
    auto rows = storage_.readAllRows(current_db_, q.table_name);

    int affected = 0;
    for (auto& row : rows) {
        bool match = !q.where || evalWhere(*q.where, row, schema);
        if (match) {
            for (const auto& sc : q.set_clauses) {
                int idx = colIndex(schema, sc.column);
                if (idx < 0) return err("Unknown column: " + sc.column);
                row[idx] = sc.value;
            }
            ++affected;
        }
    }

    storage_.writeAllRows(current_db_, q.table_name, rows);

    json result;
    result["success"] = true;
    result["type"] = "modify";
    result["affected_rows"] = affected;
    result["message"] = std::to_string(affected) + " row(s) updated.";
    return result;
}

// ── DELETE ─────────────────────────────────────────────────────────────

json Executor::execDelete(const ParsedQuery& q) {
    requireDB();
    auto schema = storage_.getTableSchema(current_db_, q.table_name);
    auto rows = storage_.readAllRows(current_db_, q.table_name);

    std::vector<Row> kept;
    int deleted = 0;
    for (const auto& row : rows) {
        bool match = !q.where || evalWhere(*q.where, row, schema);
        if (match) ++deleted;
        else kept.push_back(row);
    }

    storage_.writeAllRows(current_db_, q.table_name, kept);

    json result;
    result["success"] = true;
    result["type"] = "modify";
    result["affected_rows"] = deleted;
    result["message"] = std::to_string(deleted) + " row(s) deleted.";
    return result;
}

} // namespace db
