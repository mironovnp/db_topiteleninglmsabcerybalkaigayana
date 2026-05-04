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
    // Exact match first (handles "table.col" and bare names)
    for (size_t i = 0; i < s.columns.size(); ++i)
        if (s.columns[i].name == name) return static_cast<int>(i);
    // Bare-name fallback: match column whose name ends with ".name"
    for (size_t i = 0; i < s.columns.size(); ++i) {
        const auto& cn = s.columns[i].name;
        auto dot = cn.rfind('.');
        if (dot != std::string::npos && cn.substr(dot + 1) == name)
            return static_cast<int>(i);
    }
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
            case QueryType::ALTER_TABLE:     return execAlterTable(query);
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

// ── ALTER TABLE ────────────────────────────────────────────────────

json Executor::execAlterTable(const ParsedQuery& q) {
    requireDB();
    if (!storage_.tableExists(current_db_, q.table_name))
        return err("Table '" + q.table_name + "' does not exist.");
    ColumnDef cd{ q.alter_col_name, q.alter_col_type };
    if (storage_.alterTableAddColumn(current_db_, q.table_name, cd))
        return ok("Column '" + q.alter_col_name + "' added to '" + q.table_name + "'.");
    return err("Column '" + q.alter_col_name + "' already exists or alter failed.");
}

// ── SELECT ───────────────────────────────────────────────────────

json Executor::execSelect(const ParsedQuery& q) {
    requireDB();
    auto schema = storage_.getTableSchema(current_db_, q.table_name);
    auto rows   = storage_.readAllRows(current_db_, q.table_name);

    // ───────────────────────────────────────────────────────────────
    // JOIN processing
    // ───────────────────────────────────────────────────────────────
    // Add table prefix to all column names so we can resolve table.col
    // merged_schema holds all columns with names prefixed as "table.col"
    // but also keeps bare column name for backward compat (first owner wins).
    TableSchema merged = schema;
    // prefix left table columns with table_name
    for (auto& col : merged.columns)
        col.name = q.table_name + "." + col.name;

    for (const auto& jc : q.joins) {
        if (!storage_.tableExists(current_db_, jc.table_name))
            return err("JOIN table '" + jc.table_name + "' does not exist.");

        auto rschema = storage_.getTableSchema(current_db_, jc.table_name);

        // Determine which column is the PK of right table and if join is by PK
        // ON left_col = right_col  — detect which side is PK of right table
        // left_col / right_col have .table and .column fields
        // We figure out: for each left row, what key to look up in right table
        // Case 1: right_col.table == jc.table_name (or empty) && right_col.column is PK of right
        // Case 2: left_col.table  == jc.table_name                     — swapped

        auto colName = [](const QualifiedCol& qc){ return qc.column; };

        bool fast_path = false;
        bool swapped   = false; // true means lookup key comes from right row, scan right

        auto isRightPK = [&](const std::string& colname) {
            return rschema.columns[rschema.primary_key_index].name == colname;
        };

        // Check: right_col refers to right table PK => fast lookup in right
        if ((jc.right_col.table.empty() || jc.right_col.table == jc.table_name) &&
             isRightPK(jc.right_col.column)) {
            fast_path = true; swapped = false;
        } else if ((jc.left_col.table.empty() || jc.left_col.table == jc.table_name) &&
                    isRightPK(jc.left_col.column)) {
            fast_path = true; swapped = true;
        }

        // Find index of the join column in the left (merged) rows
        // left col name (unqualified)
        std::string left_join_col  = swapped ? colName(jc.right_col) : colName(jc.left_col);
        std::string left_join_tbl  = swapped ? jc.right_col.table    : jc.left_col.table;
        std::string right_join_col = swapped ? colName(jc.left_col)  : colName(jc.right_col);

        // Find left column index in merged schema
        auto findMergedIdx = [&](const std::string& tbl, const std::string& col) -> int {
            // Try qualified name first
            for (int i = 0; i < (int)merged.columns.size(); ++i) {
                const std::string& cn = merged.columns[i].name;
                // cn is like "tablename.col"
                auto dot = cn.find('.');
                std::string ctbl = (dot != std::string::npos) ? cn.substr(0, dot) : "";
                std::string ccol = (dot != std::string::npos) ? cn.substr(dot+1) : cn;
                if (!tbl.empty() && ctbl == tbl && ccol == col) return i;
                if (tbl.empty() && ccol == col) return i;
            }
            return -1;
        };

        int left_idx = findMergedIdx(left_join_tbl, left_join_col);
        if (left_idx < 0)
            return err("JOIN ON: unknown column '" + left_join_col + "'");

        // null row for right table (used in LEFT JOIN)
        Row null_right(rschema.columns.size(), "");

        std::vector<Row> right_rows;
        if (!fast_path)
            right_rows = storage_.readAllRows(current_db_, jc.table_name);

        // For RIGHT JOIN we need to track which right rows were matched
        std::vector<bool> right_matched;
        if (jc.join_type == JoinClause::RIGHT) {
            if (fast_path)
                right_rows = storage_.readAllRows(current_db_, jc.table_name);
            right_matched.assign(right_rows.size(), false);
        }

        // Find right column index in rschema (for fallback scan)
        int right_idx = -1;
        if (!fast_path) {
            for (int i = 0; i < (int)rschema.columns.size(); ++i)
                if (rschema.columns[i].name == right_join_col) { right_idx = i; break; }
            if (right_idx < 0)
                return err("JOIN ON: unknown column '" + right_join_col + "' in '" + jc.table_name + "'");
        }

        std::vector<Row> joined_rows;

        for (size_t li = 0; li < rows.size(); ++li) {
            const Row& lrow = rows[li];
            const std::string& key = lrow[left_idx];

            std::vector<Row> matches;

            if (fast_path) {
                // O(log N) B+ lookup
                Row found = storage_.findRow(current_db_, jc.table_name, key);
                if (!found.empty()) {
                    if (jc.join_type == JoinClause::RIGHT) {
                        // Mark matched right row
                        for (size_t ri = 0; ri < right_rows.size(); ++ri)
                            if (right_rows[ri][rschema.primary_key_index] == key) {
                                right_matched[ri] = true; break;
                            }
                    }
                    matches.push_back(std::move(found));
                }
            } else {
                // Full scan fallback
                for (size_t ri = 0; ri < right_rows.size(); ++ri) {
                    if (right_rows[ri][right_idx] == key) {
                        if (jc.join_type == JoinClause::RIGHT)
                            right_matched[ri] = true;
                        matches.push_back(right_rows[ri]);
                    }
                }
            }

            if (matches.empty()) {
                if (jc.join_type == JoinClause::LEFT) {
                    // LEFT JOIN: emit left row + null right
                    Row combined = lrow;
                    combined.insert(combined.end(), null_right.begin(), null_right.end());
                    joined_rows.push_back(std::move(combined));
                }
                // INNER / RIGHT: skip unmatched left rows here
            } else {
                for (const auto& rrow : matches) {
                    Row combined = lrow;
                    combined.insert(combined.end(), rrow.begin(), rrow.end());
                    joined_rows.push_back(std::move(combined));
                }
            }
        }

        // RIGHT JOIN: emit unmatched right rows
        if (jc.join_type == JoinClause::RIGHT) {
            Row null_left(merged.columns.size(), "");
            for (size_t ri = 0; ri < right_rows.size(); ++ri) {
                if (!right_matched[ri]) {
                    Row combined = null_left;
                    combined.insert(combined.end(), right_rows[ri].begin(), right_rows[ri].end());
                    joined_rows.push_back(std::move(combined));
                }
            }
        }

        // Extend merged schema with right table columns (prefixed)
        for (const auto& col : rschema.columns) {
            ColumnDef cd;
            cd.name = jc.table_name + "." + col.name;
            cd.type = col.type;
            merged.columns.push_back(cd);
        }

        rows = std::move(joined_rows);
    }
    // End JOIN processing

    // Helper: resolve column name (possibly "tbl.col") in merged schema
    // Returns index in merged.columns, or -1 if not found
    auto resolveCol = [&](const std::string& name) -> int {
        // Try exact match (e.g., "users.id")
        for (int i = 0; i < (int)merged.columns.size(); ++i)
            if (merged.columns[i].name == name) return i;
        // Try bare column name (last part after dot in schema)
        for (int i = 0; i < (int)merged.columns.size(); ++i) {
            auto dot = merged.columns[i].name.rfind('.');
            std::string bare = (dot != std::string::npos)
                               ? merged.columns[i].name.substr(dot+1)
                               : merged.columns[i].name;
            if (bare == name) return i;
        }
        return -1;
    };


    // 1. Filter by WHERE
    if (q.where) {
        std::vector<Row> filtered;
        for (const auto& row : rows)
            if (evalWhere(*q.where, row, merged))
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
                int idx = resolveCol(gb);
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

                int idx = resolveCol(col);
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
             empty_gd.rep_row = Row(merged.columns.size(), "");
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
                if (evalHaving(*q.having, gd.rep_row, merged, gd.aggrs))
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
        for (const auto& col : merged.columns) {
            SelectColumn sc; sc.name = col.name;
            final_select.push_back(sc);
            // Display name: strip table prefix for readability
            auto dot = col.name.rfind('.');
            colNames.push_back(dot != std::string::npos ? col.name.substr(dot+1) : col.name);
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
                int idx = resolveCol(sc.name);
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
                    int sidx = resolveCol(final_select[idx].name);
                    if (sidx >= 0) type = merged.columns[sidx].type;
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
