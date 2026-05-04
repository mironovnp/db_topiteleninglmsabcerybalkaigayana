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
    // Bare-name fallback: match schema column whose name ends with ".name"
    for (size_t i = 0; i < s.columns.size(); ++i) {
        const auto& cn = s.columns[i].name;
        auto dot = cn.rfind('.');
        if (dot != std::string::npos && cn.substr(dot + 1) == name)
            return static_cast<int>(i);
    }
    // Qualified-name fallback: if name is "table.col" and schema is "table" with column "col"
    auto dot = name.find('.');
    if (dot != std::string::npos && !s.table_name.empty()) {
        std::string tname = name.substr(0, dot);
        std::string cname = name.substr(dot + 1);
        if (tname == s.table_name) {
            for (size_t i = 0; i < s.columns.size(); ++i)
                if (s.columns[i].name == cname) return static_cast<int>(i);
        }
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
                          const TableSchema& schema,
                          const TableSchema* outer_schema,
                          const Row* outer_row) const {
    switch (expr.kind) {
    case WhereExpr::CMP: {
        int idx = colIndex(schema, expr.column);
        std::string val;
        std::string type;
        if (idx >= 0) {
            val = row[idx];
            type = schema.columns[idx].type;
        } else if (outer_schema && (idx = colIndex(*outer_schema, expr.column)) >= 0) {
            val = (*outer_row)[idx];
            type = outer_schema->columns[idx].type;
        } else {
            throw std::runtime_error("Unknown column: " + expr.column);
        }

        // Determine right hand side: either a column value or a literal
        std::string right_val = expr.value;
        int right_idx = colIndex(schema, expr.value);
        if (right_idx >= 0) {
            right_val = row[right_idx];
        } else if (outer_schema && (right_idx = colIndex(*outer_schema, expr.value)) >= 0) {
            right_val = (*outer_row)[right_idx];
        } else if (expr.value.find('.') != std::string::npos) {
            std::string debug = "Failed to resolve RHS: " + expr.value;
            if (outer_schema) {
                debug += " | outer_schema cols:";
                for (const auto& c : outer_schema->columns) debug += " " + c.name;
            } else {
                debug += " | NO outer_schema";
            }
            throw std::runtime_error(debug);
        }

        int cmp = compareValues(val, right_val, type);
        if (expr.op == "=")  return cmp == 0;
        if (expr.op == "!=") return cmp != 0;
        if (expr.op == "<")  return cmp < 0;
        if (expr.op == ">")  return cmp > 0;
        if (expr.op == "<=") return cmp <= 0;
        if (expr.op == ">=") return cmp >= 0;
        return false;
    }
    case WhereExpr::AND_OP:
        return evalWhere(*expr.left, row, schema, outer_schema, outer_row) &&
               evalWhere(*expr.right, row, schema, outer_schema, outer_row);
    case WhereExpr::OR_OP:
        return evalWhere(*expr.left, row, schema, outer_schema, outer_row) ||
               evalWhere(*expr.right, row, schema, outer_schema, outer_row);
    case WhereExpr::NOT_OP:
        return !evalWhere(*expr.left, row, schema, outer_schema, outer_row);
    case WhereExpr::IN_OP: {
        int idx = colIndex(schema, expr.column);
        std::string val;
        if (idx >= 0) val = row[idx];
        else if (outer_schema && (idx = colIndex(*outer_schema, expr.column)) >= 0) val = (*outer_row)[idx];
        else throw std::runtime_error("Unknown column: " + expr.column);

        bool found = false;
        if (expr.subquery) {
            // IN (SELECT ...)
            auto sub_result = const_cast<Executor*>(this)->execute_subquery(*expr.subquery, &schema, &row);
            for (const auto& srow : sub_result)
                if (!srow.empty() && srow[0] == val) { found = true; break; }
        } else {
            for (const auto& v : expr.in_values)
                if (v == val) { found = true; break; }
        }
        return expr.negated ? !found : found;
    }
    case WhereExpr::EXISTS_OP: {
        if (!expr.subquery) return false;
        auto sub_result = const_cast<Executor*>(this)->execute_subquery(*expr.subquery, &schema, &row);
        bool exists = !sub_result.empty();
        return expr.negated ? !exists : exists;
    }
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
    for (const auto& cd : q.column_defs) {
        ColumnDef col;
        col.name = cd.name;
        col.type = cd.type;
        col.not_null = cd.not_null;
        col.unique = cd.unique;
        col.has_default = cd.has_default;
        col.default_value = cd.default_value;
        col.fk_ref_table = cd.fk_ref_table;
        col.fk_ref_column = cd.fk_ref_column;
        schema.columns.push_back(col);
    }
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

    if (q.alter_action == AlterAction::ADD_COL) {
        ColumnDef cd{ q.alter_col_name, q.alter_col_type };
        if (storage_.alterTableAddColumn(current_db_, q.table_name, cd))
            return ok("Column '" + q.alter_col_name + "' added to '" + q.table_name + "'.");
        return err("Column '" + q.alter_col_name + "' already exists or alter failed.");
    } else {
        // DROP COLUMN
        if (storage_.alterTableDropColumn(current_db_, q.table_name, q.alter_col_name))
            return ok("Column '" + q.alter_col_name + "' dropped from '" + q.table_name + "'.");
        return err("Cannot drop column '" + q.alter_col_name + "' (not found or is primary key).");
    }
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
            if (evalWhere(*q.where, row, merged, outer_schema_, outer_row_))
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

    // 4. ORDER BY
    if (!q.order_by.empty()) {
        // Pre-validate ORDER BY columns
        for (const auto& ob : q.order_by) {
            if (ob.aggr != AggrFunc::NONE) continue;
            int proj_idx = -1;
            for (size_t i = 0; i < final_select.size(); ++i) {
                if ((!final_select[i].alias.empty() && final_select[i].alias == ob.column) ||
                    final_select[i].name == ob.column ||
                    colNames[i] == ob.column) {
                    proj_idx = static_cast<int>(i);
                    break;
                }
            }
            if (proj_idx < 0) {
                int sidx = resolveCol(ob.column);
                if (sidx < 0) return err("ORDER BY column not found: " + ob.column);
            }
        }

        std::sort(groups.begin(), groups.end(), [&](const GroupData& a, const GroupData& b) {
            for (const auto& ob : q.order_by) {
                std::string valA, valB, type = "TEXT";
                if (ob.aggr != AggrFunc::NONE) {
                    type = "FLOAT";
                    auto getAggr = [&](const GroupData& g) -> std::string {
                        auto it = g.aggrs.find({ob.aggr, ob.column});
                        if (it == g.aggrs.end()) return "0";
                        if (ob.aggr == AggrFunc::COUNT) return std::to_string(it->second.count);
                        if (ob.aggr == AggrFunc::SUM) return std::to_string(it->second.sum);
                        if (ob.aggr == AggrFunc::AVG) return std::to_string(it->second.count ? it->second.sum / it->second.count : 0);
                        if (ob.aggr == AggrFunc::MIN) return std::to_string(it->second.min_val);
                        if (ob.aggr == AggrFunc::MAX) return std::to_string(it->second.max_val);
                        return "0";
                    };
                    valA = getAggr(a); valB = getAggr(b);
                } else {
                    int proj_idx = -1;
                    for (size_t i = 0; i < final_select.size(); ++i) {
                        if ((!final_select[i].alias.empty() && final_select[i].alias == ob.column) ||
                            final_select[i].name == ob.column ||
                            colNames[i] == ob.column) {
                            proj_idx = static_cast<int>(i); break;
                        }
                    }
                    if (proj_idx >= 0) {
                        auto evalProj = [&](const GroupData& g) -> std::string {
                            const auto& sc = final_select[proj_idx];
                            if (sc.aggr != AggrFunc::NONE) {
                                auto it = g.aggrs.find({sc.aggr, sc.name});
                                if (it != g.aggrs.end()) {
                                    if (sc.aggr == AggrFunc::COUNT) return std::to_string(it->second.count);
                                    if (sc.aggr == AggrFunc::SUM) return std::to_string(it->second.sum);
                                    if (sc.aggr == AggrFunc::AVG) return std::to_string(it->second.count ? it->second.sum / it->second.count : 0);
                                    if (sc.aggr == AggrFunc::MIN) return std::to_string(it->second.min_val);
                                    if (sc.aggr == AggrFunc::MAX) return std::to_string(it->second.max_val);
                                }
                                return "0";
                            } else {
                                int sidx = resolveCol(sc.name);
                                if (sidx >= 0) return g.rep_row[sidx];
                                return "";
                            }
                        };
                        valA = evalProj(a); valB = evalProj(b);
                        const auto& sc = final_select[proj_idx];
                        if (sc.aggr != AggrFunc::NONE) type = "FLOAT";
                        else {
                            int sidx = resolveCol(sc.name);
                            if (sidx >= 0) type = merged.columns[sidx].type;
                        }
                    } else {
                        int sidx = resolveCol(ob.column);
                        if (sidx >= 0) {
                            valA = a.rep_row[sidx]; valB = b.rep_row[sidx];
                            type = merged.columns[sidx].type;
                        }
                    }
                }
                
                int cmp = compareValues(valA, valB, type);
                if (cmp != 0) {
                    return ob.asc ? (cmp < 0) : (cmp > 0);
                }
            }
            return false;
        });
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

    // 6. Apply LIMIT / OFFSET
    if (q.limit >= 0) {
        int start = q.offset;
        int end = std::min(start + q.limit, (int)result_rows.size());
        if (start >= (int)result_rows.size()) {
            result["rows"] = json::array();
            result["message"] = "0 row(s) returned.";
        } else {
            json limited = json::array();
            for (int i = start; i < end; ++i) {
                json jr = json::array();
                for (const auto& val : result_rows[i]) jr.push_back(val);
                limited.push_back(jr);
            }
            result["rows"] = limited;
            result["message"] = std::to_string(end - start) + " row(s) returned.";
        }
    }

    return result;
}

// ── Subquery execution ────────────────────────────────────────────────

std::vector<Row> Executor::execute_subquery(const ParsedQuery& q,
                                            const TableSchema* outer_schema,
                                            const Row* outer_row) {
    // Save current outer context
    const TableSchema* old_schema = outer_schema_;
    const Row* old_row = outer_row_;
    outer_schema_ = outer_schema;
    outer_row_ = outer_row;

    auto result = execSelect(q);

    // Restore context
    outer_schema_ = old_schema;
    outer_row_ = old_row;

    std::vector<Row> rows;
    if (result.contains("rows") && result["rows"].is_array()) {
        for (const auto& jr : result["rows"]) {
            Row row;
            for (const auto& val : jr) row.push_back(val.get<std::string>());
            rows.push_back(std::move(row));
        }
    }
    return rows;
}

// ── INSERT ─────────────────────────────────────────────────────────────

json Executor::execInsert(const ParsedQuery& q) {
    requireDB();
    auto schema = storage_.getTableSchema(current_db_, q.table_name);

    std::vector<Row> rows;
    for (const auto& vals : q.insert_values) {
        if (!q.insert_columns.empty()) {
            // Map named columns to row positions, apply defaults
            Row row(schema.columns.size(), "");
            // Fill defaults first
            for (size_t c = 0; c < schema.columns.size(); ++c)
                if (schema.columns[c].has_default)
                    row[c] = schema.columns[c].default_value;
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
            Row row = vals;
            // Apply defaults for empty values
            for (size_t c = 0; c < schema.columns.size(); ++c)
                if (row[c].empty() && schema.columns[c].has_default)
                    row[c] = schema.columns[c].default_value;
            rows.push_back(std::move(row));
        }
    }

    // Constraint checks
    auto existing = storage_.readAllRows(current_db_, q.table_name);

    for (const auto& row : rows) {
        for (size_t c = 0; c < schema.columns.size(); ++c) {
            const auto& col = schema.columns[c];
            // NOT NULL
            if (col.not_null && row[c].empty())
                return err("NOT NULL constraint violated for column '" + col.name + "'.");
            // UNIQUE
            if (col.unique && !row[c].empty()) {
                for (const auto& er : existing)
                    if (c < er.size() && er[c] == row[c])
                        return err("UNIQUE constraint violated for column '" + col.name + "': value '" + row[c] + "'.");
            }
            // FOREIGN KEY
            if (!col.fk_ref_table.empty()) {
                if (!storage_.tableExists(current_db_, col.fk_ref_table))
                    return err("FK: referenced table '" + col.fk_ref_table + "' does not exist.");
                auto ref_schema = storage_.getTableSchema(current_db_, col.fk_ref_table);
                int ref_idx = -1;
                for (size_t r = 0; r < ref_schema.columns.size(); ++r)
                    if (ref_schema.columns[r].name == col.fk_ref_column) { ref_idx = (int)r; break; }
                if (ref_idx < 0)
                    return err("FK: referenced column '" + col.fk_ref_column + "' not found.");
                // Check value exists in referenced table (use B+ if PK, else scan)
                bool found = false;
                if (ref_idx == ref_schema.primary_key_index) {
                    Row fr = storage_.findRow(current_db_, col.fk_ref_table, row[c]);
                    found = !fr.empty();
                } else {
                    auto ref_rows = storage_.readAllRows(current_db_, col.fk_ref_table);
                    for (const auto& rr : ref_rows)
                        if (ref_idx < (int)rr.size() && rr[ref_idx] == row[c]) { found = true; break; }
                }
                if (!found && !row[c].empty())
                    return err("FK constraint violated: value '" + row[c] + "' not found in " +
                               col.fk_ref_table + "(" + col.fk_ref_column + ").");
            }
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
