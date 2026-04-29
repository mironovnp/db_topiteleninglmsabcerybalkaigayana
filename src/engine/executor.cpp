#include "engine/executor.hpp"
#include <stdexcept>
#include <algorithm>
#include <functional>

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

bool Executor::evalHaving(const WhereExpr& expr, const Row& row, const TableSchema& schema,
                          const std::map<std::pair<AggrFunc, std::string>, AggrState>& aggrs) const {
    switch (expr.kind) {
    case WhereExpr::CMP: {
        std::string val;
        std::string type = "FLOAT"; // Default to float for aggregates
        if (expr.aggr != AggrFunc::NONE) {
            auto it = aggrs.find({expr.aggr, expr.column});
            if (it != aggrs.end()) {
                if (expr.aggr == AggrFunc::COUNT) val = std::to_string(it->second.count);
                else if (expr.aggr == AggrFunc::SUM) val = std::to_string(it->second.sum);
                else if (expr.aggr == AggrFunc::AVG) val = std::to_string(it->second.count ? it->second.sum / it->second.count : 0);
                else if (expr.aggr == AggrFunc::MIN) val = std::to_string(it->second.min_val);
                else if (expr.aggr == AggrFunc::MAX) val = std::to_string(it->second.max_val);
            } else {
                val = "0"; // Default
            }
        } else {
            int idx = colIndex(schema, expr.column);
            if (idx < 0) throw std::runtime_error("Unknown column in HAVING: " + expr.column);
            val = row[idx];
            type = schema.columns[idx].type;
        }

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
        return evalHaving(*expr.left, row, schema, aggrs) &&
               evalHaving(*expr.right, row, schema, aggrs);
    case WhereExpr::OR_OP:
        return evalHaving(*expr.left, row, schema, aggrs) ||
               evalHaving(*expr.right, row, schema, aggrs);
    case WhereExpr::NOT_OP:
        return !evalHaving(*expr.left, row, schema, aggrs);
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
        case QueryType::USE_DATABASE:    return execUse(query);
        default:
            requireDB();
            switch (query.type) {
            case QueryType::CREATE_TABLE:    return execCreateTable(query);
            case QueryType::DROP_TABLE:      return execDropTable(query);
            case QueryType::SELECT:          return execSelect(query);
            case QueryType::INSERT:          return execInsert(query);
            case QueryType::UPDATE:          return execUpdate(query);
            case QueryType::DELETE_Q:        return execDelete(query);
            default: return err("Unknown query type");
            }
        }
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
    // PRIMARY KEY: use specified index, or default to 0 (first column)
    schema.primary_key_index = (q.primary_key_index >= 0) ? q.primary_key_index : 0;

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

    // 1. Filter by WHERE
    if (q.where) {
        std::vector<Row> filtered;
        for (const auto& row : rows)
            if (evalWhere(*q.where, row, schema))
                filtered.push_back(row);
        rows = std::move(filtered);
    }

    // Determine if we need to group (GROUP BY clause or presence of aggregates in SELECT)
    bool needs_grouping = !q.group_by.empty();
    if (!needs_grouping) {
        for (const auto& sc : q.select_columns) {
            if (sc.aggr != AggrFunc::NONE) { needs_grouping = true; break; }
        }
    }

    struct GroupData {
        Row rep_row;
        std::map<std::pair<AggrFunc, std::string>, AggrState> aggrs;
    };
    std::vector<GroupData> groups;

    if (needs_grouping) {
        std::map<std::string, GroupData> group_map;
        // Collect necessary aggregates from select_columns and having
        std::vector<std::pair<AggrFunc, std::string>> needed_aggrs;
        for (const auto& sc : q.select_columns) {
            if (sc.aggr != AggrFunc::NONE) needed_aggrs.push_back(std::make_pair(sc.aggr, sc.name));
        }
        std::function<void(const WhereExpr*)> extractHavingAggrs = [&](const WhereExpr* expr) {
            if (!expr) return;
            if (expr->kind == WhereExpr::CMP) {
                if (expr->aggr != AggrFunc::NONE) needed_aggrs.push_back(std::make_pair(expr->aggr, expr->column));
            } else {
                extractHavingAggrs(expr->left.get());
                extractHavingAggrs(expr->right.get());
            }
        };
        if (q.having) extractHavingAggrs(q.having.get());

        for (const auto& row : rows) {
            // Compute group key
            std::string key;
            for (const auto& gb : q.group_by) {
                int idx = colIndex(schema, gb);
                if (idx < 0) return err("Unknown column in GROUP BY: " + gb);
                key += row[idx] + "|";
            }

            auto& gd = group_map[key];
            if (!gd.rep_row.size()) gd.rep_row = row;

            for (const auto& ag : needed_aggrs) {
                AggrFunc func = ag.first;
                std::string col = ag.second;
                auto& state = gd.aggrs[{func, col}];
                state.count++;
                if (col == "*") continue; // COUNT(*) handled by state.count

                int idx = colIndex(schema, col);
                if (idx < 0) {
                    throw std::runtime_error("Unknown column in aggregate: " + col);
                }
                std::string val = row[idx];
                double dval = 0;
                if (!val.empty()) {
                    try { dval = std::stod(val); } catch (...) {}
                }

                if (!state.initialized) {
                    state.sum = state.min_val = state.max_val = dval;
                    state.initialized = true;
                } else {
                    state.sum += dval;
                    if (dval < state.min_val) state.min_val = dval;
                    if (dval > state.max_val) state.max_val = dval;
                }
            }
        }

        // Output groups
        if (group_map.empty() && q.group_by.empty()) {
             // Handle queries like SELECT COUNT(*) FROM empty_table
             GroupData empty_gd;
             empty_gd.rep_row = Row(schema.columns.size(), "");
             for (const auto& ag : needed_aggrs) {
                 empty_gd.aggrs[ag] = AggrState();
             }
             groups.push_back(empty_gd);
        } else {
            for (auto& kv : group_map) groups.push_back(std::move(kv.second));
        }
    } else {
        // No grouping, each row is its own group
        for (const auto& row : rows) {
            GroupData gd; gd.rep_row = row;
            groups.push_back(std::move(gd));
        }
    }

    // 2. Filter by HAVING
    if (q.having && needs_grouping) {
        std::vector<GroupData> filtered;
        for (const auto& gd : groups) {
            try {
                if (evalHaving(*q.having, gd.rep_row, schema, gd.aggrs))
                    filtered.push_back(gd);
            } catch (const std::exception& e) {
                return err(e.what());
            }
        }
        groups = std::move(filtered);
    }

    // 3. Evaluate projections into a tabular format
    std::vector<std::string> colNames;
    std::vector<SelectColumn> final_select;
    if (q.select_all) {
        for (const auto& col : schema.columns) {
            SelectColumn sc; sc.name = col.name;
            final_select.push_back(sc);
            colNames.push_back(col.name);
        }
    } else {
        for (const auto& sc : q.select_columns) {
            final_select.push_back(sc);
            std::string colName = sc.name;
            if (!sc.alias.empty()) {
                colName = sc.alias;
            } else if (sc.aggr != AggrFunc::NONE) {
                std::string funcName;
                switch (sc.aggr) {
                    case AggrFunc::COUNT: funcName = "COUNT"; break;
                    case AggrFunc::SUM: funcName = "SUM"; break;
                    case AggrFunc::AVG: funcName = "AVG"; break;
                    case AggrFunc::MIN: funcName = "MIN"; break;
                    case AggrFunc::MAX: funcName = "MAX"; break;
                    default: funcName = "AGGR"; break;
                }
                colName = funcName + "(" + sc.name + ")";
            }
            colNames.push_back(colName);
        }
    }

    std::vector<Row> result_rows;
    for (const auto& gd : groups) {
        Row res_row;
        for (const auto& sc : final_select) {
            if (sc.aggr != AggrFunc::NONE) {
                auto it = gd.aggrs.find({sc.aggr, sc.name});
                if (it != gd.aggrs.end()) {
                    if (sc.aggr == AggrFunc::COUNT) res_row.push_back(std::to_string(it->second.count));
                    else if (sc.aggr == AggrFunc::SUM) res_row.push_back(std::to_string(it->second.sum));
                    else if (sc.aggr == AggrFunc::AVG) res_row.push_back(std::to_string(it->second.count ? it->second.sum / it->second.count : 0));
                    else if (sc.aggr == AggrFunc::MIN) res_row.push_back(std::to_string(it->second.min_val));
                    else if (sc.aggr == AggrFunc::MAX) res_row.push_back(std::to_string(it->second.max_val));
                } else {
                    res_row.push_back("0");
                }
            } else {
                int idx = colIndex(schema, sc.name);
                if (idx < 0) return err("Unknown column: " + sc.name);
                res_row.push_back(gd.rep_row[idx]);
            }
        }
        result_rows.push_back(std::move(res_row));
    }

    // 4. ORDER BY
    if (!q.order_by.empty()) {
        std::vector<std::pair<int, bool>> sort_criteria; // {result_col_index, is_asc}
        for (const auto& ob : q.order_by) {
            int idx = -1;
            for (size_t i = 0; i < final_select.size(); ++i) {
                if (final_select[i].aggr == ob.aggr && (
                    (!final_select[i].alias.empty() && final_select[i].alias == ob.column) || 
                    final_select[i].name == ob.column ||
                    colNames[i] == ob.column)) {
                    idx = static_cast<int>(i); break;
                }
            }
            if (idx < 0) return err("ORDER BY column not in SELECT: " + ob.column);
            sort_criteria.push_back({idx, ob.asc});
        }

        std::sort(result_rows.begin(), result_rows.end(), [&](const Row& a, const Row& b) {
            for (const auto& crit : sort_criteria) {
                int idx = crit.first;
                std::string type = "TEXT";
                if (final_select[idx].aggr != AggrFunc::NONE) type = "FLOAT";
                else {
                    int sidx = colIndex(schema, final_select[idx].name);
                    if (sidx >= 0) type = schema.columns[sidx].type;
                }

                int cmp = compareValues(a[idx], b[idx], type);
                if (cmp != 0) {
                    return crit.second ? (cmp < 0) : (cmp > 0);
                }
            }
            return false;
        });
    }

    // 5. Build result
    json result;
    result["success"] = true;
    result["type"] = "select";
    result["columns"] = colNames;
    result["rows"] = json::array();

    for (const auto& row : result_rows) {
        json jr = json::array();
        for (const auto& val : row) jr.push_back(val);
        result["rows"].push_back(jr);
    }
    result["message"] = std::to_string(result_rows.size()) + " row(s) returned.";
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
