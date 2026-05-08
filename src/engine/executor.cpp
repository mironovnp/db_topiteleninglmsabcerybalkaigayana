#include "engine/executor.hpp"
#include <stdexcept>
#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <iomanip>
#include <sstream>

#include <regex>
#include <iostream>

namespace db {

static std::string formatFloat(double val) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(3) << val;
    return out.str();
}

static bool likeMatch(const std::string& str, std::string pattern) {
    std::string rx;
    for (char c : pattern) {
        if (c == '%') rx += ".*";
        else if (c == '_') rx += ".";
        else if (std::string(".+*?^$()[]{}|\\").find(c) != std::string::npos) {
            rx += "\\"; rx += c;
        }
        else rx += c;
    }
    try {
        std::regex re("^" + rx + "$", std::regex_constants::icase);
        return std::regex_match(str, re);
    } catch (...) { return false; }
}

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

int Executor::colIndex(const TableSchema& s, const std::string& table, const std::string& name) const {
    for (size_t i = 0; i < s.columns.size(); ++i) {
        if (!table.empty() && s.columns[i].name == table + "." + name) return static_cast<int>(i);
        if (table.empty() && s.columns[i].name == name) return static_cast<int>(i);
    }
    for (size_t i = 0; i < s.columns.size(); ++i) {
        auto dot = s.columns[i].name.rfind('.');
        std::string bare = (dot != std::string::npos) ? s.columns[i].name.substr(dot + 1) : s.columns[i].name;
        if (bare == name) {
            if (table.empty() || (!s.table_name.empty() && table == s.table_name)) return static_cast<int>(i);
            std::string tbl = (dot != std::string::npos) ? s.columns[i].name.substr(0, dot) : "";
            if (tbl == table) return static_cast<int>(i);
        }
    }
    return -1;
}

int Executor::compareValues(const Value& a, const Value& b) const {
    if (a.type == "NULL" || b.type == "NULL") {
        if (a.type == "NULL" && b.type == "NULL") return 0;
        return (a.type == "NULL") ? -1 : 1;
    }
    std::string t = (a.type == "FLOAT" || b.type == "FLOAT") ? "FLOAT" :
                    (a.type == "INT" || b.type == "INT") ? "INT" : "TEXT";
    if (t == "INT") {
        try {
            long la = a.val.empty() ? 0 : std::stol(a.val);
            long lb = b.val.empty() ? 0 : std::stol(b.val);
            return (la < lb) ? -1 : (la > lb) ? 1 : 0;
        } catch (...) { return a.val.compare(b.val); }
    }
    if (t == "FLOAT") {
        try {
            double da = a.val.empty() ? 0.0 : std::stod(a.val);
            double db = b.val.empty() ? 0.0 : std::stod(b.val);
            return (da < db) ? -1 : (da > db) ? 1 : 0;
        } catch (...) { return a.val.compare(b.val); }
    }
    return a.val.compare(b.val);
}

Value Executor::evaluateExpression(const Expression* expr, const Row& row, const TableSchema& schema,
                                   const std::map<std::pair<AggrFunc, std::string>, AggrState>* aggrs,
                                   const TableSchema* outer_schema, const Row* outer_row) {
    if (!expr) return {"", "NULL"};
    if (auto lit = dynamic_cast<const LiteralExpression*>(expr)) {
        if (lit->type == TokenType::NUMBER_LITERAL) return {lit->value, lit->value.find('.') != std::string::npos ? "FLOAT" : "INT"};
        if (lit->type == TokenType::BOOL_LITERAL) return {lit->value == "TRUE" ? "1" : "0", "BOOL"};
        if (lit->type == TokenType::KW_NULL) return {"", "NULL"};
        return {lit->value, "TEXT"};
    }
    if (auto col = dynamic_cast<const ColumnExpression*>(expr)) {
        int idx = colIndex(schema, col->table, col->column);
        if (idx >= 0) return {row[idx], schema.columns[idx].type};
        if (outer_schema && outer_row) {
            idx = colIndex(*outer_schema, col->table, col->column);
            if (idx >= 0) return {(*outer_row)[idx], outer_schema->columns[idx].type};
        }
        throw std::runtime_error("Unknown column: " + (col->table.empty() ? "" : col->table + ".") + col->column);
    }
    if (auto aggr = dynamic_cast<const AggregateExpression*>(expr)) {
        if (aggrs) {
            auto it = aggrs->find({aggr->func, aggr->column});
            if (it != aggrs->end()) {
                if (aggr->func == AggrFunc::COUNT) return {std::to_string(it->second.count), "INT"};
                if (aggr->func == AggrFunc::SUM) return {formatFloat(it->second.sum), "FLOAT"};
                if (aggr->func == AggrFunc::AVG) return {formatFloat(it->second.count ? it->second.sum / it->second.count : 0), "FLOAT"};
                if (aggr->func == AggrFunc::MIN) return {formatFloat(it->second.min_val), "FLOAT"};
                if (aggr->func == AggrFunc::MAX) return {formatFloat(it->second.max_val), "FLOAT"};
            }
        }
        return {"0", "FLOAT"};
    }
    if (auto un = dynamic_cast<const UnaryExpression*>(expr)) {
        if (un->op == TokenType::KW_NOT) return {evalCondition(un->operand.get(), row, schema, aggrs, outer_schema, outer_row) ? "0" : "1", "BOOL"};
        if (un->op == TokenType::OP_MINUS) {
            Value v = evaluateExpression(un->operand.get(), row, schema, aggrs, outer_schema, outer_row);
            double d = v.val.empty() ? 0 : std::stod(v.val);
            return {formatFloat(-d), "FLOAT"};
        }
    }
    if (auto bin = dynamic_cast<const BinaryExpression*>(expr)) {
        if (bin->op == TokenType::KW_AND) {
            if (!evalCondition(bin->left.get(), row, schema, aggrs, outer_schema, outer_row)) return {"0", "BOOL"};
            return {evalCondition(bin->right.get(), row, schema, aggrs, outer_schema, outer_row) ? "1" : "0", "BOOL"};
        }
        if (bin->op == TokenType::KW_OR) {
            if (evalCondition(bin->left.get(), row, schema, aggrs, outer_schema, outer_row)) return {"1", "BOOL"};
            return {evalCondition(bin->right.get(), row, schema, aggrs, outer_schema, outer_row) ? "1" : "0", "BOOL"};
        }
        Value left = evaluateExpression(bin->left.get(), row, schema, aggrs, outer_schema, outer_row);
        if (bin->op == TokenType::KW_IN) {
            if (auto sub = dynamic_cast<const SubqueryExpression*>(bin->right.get())) {
                auto sub_res = const_cast<Executor*>(this)->execute_subquery(sub->subquery.get(), &schema, &row);
                bool found = false;
                for (const auto& sr : sub_res) if (!sr.empty() && sr[0] == left.val) { found = true; break; }
                return {(sub->negated ? !found : found) ? "1" : "0", "BOOL"};
            }
        }
        Value right = evaluateExpression(bin->right.get(), row, schema, aggrs, outer_schema, outer_row);
        if (bin->op == TokenType::OP_PLUS || bin->op == TokenType::OP_MINUS || bin->op == TokenType::STAR || bin->op == TokenType::OP_DIV) {
            double lv = left.val.empty() ? 0 : std::stod(left.val);
            double rv = right.val.empty() ? 0 : std::stod(right.val);
            double res = 0;
            if (bin->op == TokenType::OP_PLUS) res = lv + rv;
            else if (bin->op == TokenType::OP_MINUS) res = lv - rv;
            else if (bin->op == TokenType::STAR) res = lv * rv;
            else if (bin->op == TokenType::OP_DIV) res = (rv != 0) ? lv / rv : 0;
            return {formatFloat(res), "FLOAT"};
        }
        int cmp = compareValues(left, right);
        bool res = false;
        switch (bin->op) {
            case TokenType::OP_EQ: res = (cmp == 0); break;
            case TokenType::OP_NEQ: res = (cmp != 0); break;
            case TokenType::OP_LT: res = (cmp < 0); break;
            case TokenType::OP_GT: res = (cmp > 0); break;
            case TokenType::OP_LTE: res = (cmp <= 0); break;
            case TokenType::OP_GTE: res = (cmp >= 0); break;
            default: break;
        }
        return {res ? "1" : "0", "BOOL"};
    }
    if (auto in_list = dynamic_cast<const InListExpression*>(expr)) {
        Value left = evaluateExpression(in_list->left.get(), row, schema, aggrs, outer_schema, outer_row);
        bool found = std::find(in_list->values.begin(), in_list->values.end(), left.val) != in_list->values.end();
        return {(in_list->negated ? !found : found) ? "1" : "0", "BOOL"};
    }
    if (auto is_null = dynamic_cast<const IsNullExpression*>(expr)) {
        Value v = evaluateExpression(is_null->operand.get(), row, schema, aggrs, outer_schema, outer_row);
        bool is_v_null = (v.type == "NULL" || v.val.empty());
        return {(is_null->is_not ? !is_v_null : is_v_null) ? "1" : "0", "BOOL"};
    }
    if (auto lk = dynamic_cast<const LikeExpression*>(expr)) {
        Value v = evaluateExpression(lk->left.get(), row, schema, aggrs, outer_schema, outer_row);
        bool res = likeMatch(v.val, lk->pattern);
        return {(lk->negated ? !res : res) ? "1" : "0", "BOOL"};
    }
    if (auto bt = dynamic_cast<const BetweenExpression*>(expr)) {
        Value v = evaluateExpression(bt->val.get(), row, schema, aggrs, outer_schema, outer_row);
        Value low = evaluateExpression(bt->low.get(), row, schema, aggrs, outer_schema, outer_row);
        Value high = evaluateExpression(bt->high.get(), row, schema, aggrs, outer_schema, outer_row);
        bool res = (compareValues(v, low) >= 0 && compareValues(v, high) <= 0);
        return {(bt->negated ? !res : res) ? "1" : "0", "BOOL"};
    }
    if (auto sub = dynamic_cast<const SubqueryExpression*>(expr)) {
        auto res = const_cast<Executor*>(this)->execute_subquery(sub->subquery.get(), &schema, &row);
        if (sub->is_exists) {
            return {(!res.empty()) ? "1" : "0", "BOOL"};
        }
        // Scalar subquery: return first column of first row
        if (res.empty() || res[0].empty()) return {"", "NULL"};
        return {res[0][0], "TEXT"}; // Type info is lost here but will be coerced in comparisons
    }
    return {"", "NULL"};
}

bool Executor::evalCondition(const Expression* expr, const Row& row, const TableSchema& schema,
                             const std::map<std::pair<AggrFunc, std::string>, AggrState>* aggrs,
                             const TableSchema* outer_schema, const Row* outer_row) {
    if (!expr) return true;
    return evaluateExpression(expr, row, schema, aggrs, outer_schema, outer_row).val == "1";
}

json Executor::execute(const std::string& sql) {
    try {
        Lexer l(sql); auto tokens = l.tokenize();

        // Check for database requirement before parsing to provide better error messages
        if (!tokens.empty() && tokens[0].type != TokenType::END_OF_INPUT) {
            bool needs_db = true;
            TokenType t0 = tokens[0].type;

            if (t0 == TokenType::KW_USE) {
                needs_db = false;
            } else if (t0 == TokenType::KW_CREATE || t0 == TokenType::KW_DROP) {
                if (tokens.size() > 1 && tokens[1].type == TokenType::KW_DATABASE) {
                    needs_db = false;
                }
            }

            if (needs_db && current_db_.empty()) {
                return err("No database selected. Use: USE <database>;");
            }
        }

        Parser p(tokens); auto query = p.parse();
        if (auto q = dynamic_cast<CreateDatabaseStatement*>(query.get())) return execCreateDB(q);
        if (auto q = dynamic_cast<DropDatabaseStatement*>(query.get())) return execDropDB(q);
        if (auto q = dynamic_cast<UseDatabaseStatement*>(query.get())) return execUse(q);

        if (auto q = dynamic_cast<CreateTableStatement*>(query.get())) return execCreateTable(q);
        if (auto q = dynamic_cast<DropTableStatement*>(query.get())) return execDropTable(q);
        if (auto q = dynamic_cast<AlterTableStatement*>(query.get())) return execAlterTable(q);
        if (auto q = dynamic_cast<CreateIndexStatement*>(query.get())) return execCreateIndex(q);
        if (auto q = dynamic_cast<DropIndexStatement*>(query.get())) return execDropIndex(q);
        if (auto q = dynamic_cast<SelectStatement*>(query.get())) return execSelect(q);
        if (auto q = dynamic_cast<InsertStatement*>(query.get())) return execInsert(q);
        if (auto q = dynamic_cast<UpdateStatement*>(query.get())) return execUpdate(q);
        if (auto q = dynamic_cast<DeleteStatement*>(query.get())) return execDelete(q);
        return err("Unknown query type");
    } catch (const std::exception& e) { return err(e.what()); }
}

json Executor::execCreateDB(const CreateDatabaseStatement* q) {
    if (storage_.databaseExists(q->database_name)) return err("Database '" + q->database_name + "' already exists.");
    if (storage_.createDatabase(q->database_name)) return ok("Database '" + q->database_name + "' created.");
    return err("Failed to create database.");
}
json Executor::execDropDB(const DropDatabaseStatement* q) {
    if (!storage_.databaseExists(q->database_name)) {
        if (q->if_exists) return ok("Dropped database '" + q->database_name + "' (if existed).");
        return err("Database '" + q->database_name + "' does not exist.");
    }
    if (storage_.dropDatabase(q->database_name)) {
        if (current_db_ == q->database_name) current_db_.clear();
        return ok("Dropped database '" + q->database_name + "'.");
    }
    return err("Failed to drop database.");
}
json Executor::execUse(const UseDatabaseStatement* q) {
    if (storage_.databaseExists(q->database_name)) {
        current_db_ = q->database_name;
        return ok("Using database '" + q->database_name + "'.");
    }
    return err("Database '" + q->database_name + "' does not exist.");
}
json Executor::execCreateTable(const CreateTableStatement* q) {
    TableSchema s; s.table_name = q->table_name;
    for (auto& cd : q->column_defs) { ColumnDef c; c.name = cd.name; c.type = cd.type; c.not_null = cd.not_null; c.unique = cd.unique; c.has_default = cd.has_default; c.is_autoincrement = cd.is_autoincrement; c.default_value = cd.default_value; c.fk_ref_table = cd.fk_ref_table; c.fk_ref_column = cd.fk_ref_column; c.on_delete = cd.on_delete; c.on_update = cd.on_update; s.columns.push_back(c); }
    s.primary_key_index = q->primary_key_index >= 0 ? q->primary_key_index : 0;
    if (storage_.createTable(current_db_, s)) return ok("Table created."); return err("Failed.");
}
json Executor::execDropTable(const DropTableStatement* q) {
    if (storage_.dropTable(current_db_, q->table_name)) return ok("Dropped.");
    if (q->if_exists) return ok("Dropped.");
    return err("Failed.");
}
json Executor::execAlterTable(const AlterTableStatement* q) {
    if (q->alter_action == AlterAction::ADD_COL) { if (storage_.alterTableAddColumn(current_db_, q->table_name, {q->alter_col_name, q->alter_col_type})) return ok("Added."); }
    else { if (storage_.alterTableDropColumn(current_db_, q->table_name, q->alter_col_name)) return ok("Dropped."); }
    return err("Failed.");
}
json Executor::execCreateIndex(const CreateIndexStatement* q) { if (storage_.createIndex(current_db_, q->table_name, q->index_name, q->column_name)) return ok("Created."); return err("Failed."); }
json Executor::execDropIndex(const DropIndexStatement* q) { if (storage_.dropIndex(current_db_, q->table_name, q->index_name)) return ok("Dropped."); return err("Failed."); }

json Executor::execInsert(const InsertStatement* q) {
    auto s = storage_.getTableSchema(current_db_, q->table_name);
    std::vector<Row> evaluated_rows;
    
    std::map<int, long> last_ids; // Reset for each INSERT statement

    for (auto& ivs : q->insert_values) {
        Row r(s.columns.size(), "");
        if (q->insert_columns.empty()) {
            for (size_t i = 0; i < ivs.size() && i < r.size(); ++i) {
                r[i] = evaluateExpression(ivs[i].get(), Row(), s).val;
            }
        } else {
            for (size_t i = 0; i < ivs.size() && i < q->insert_columns.size(); ++i) {
                int idx = colIndex(s, "", q->insert_columns[i]);
                if (idx >= 0) r[idx] = evaluateExpression(ivs[i].get(), Row(), s).val;
            }
        }

        // Apply DEFAULTS and AUTO_INCREMENT
        for (size_t i = 0; i < s.columns.size(); ++i) {
            if (r[i].empty()) {
                if (s.columns[i].is_autoincrement) {
                    if (last_ids.find(i) == last_ids.end()) {
                        long max_id = 0;
                        auto all_rows = storage_.readAllRows(current_db_, q->table_name);
                        for (const auto& ar : all_rows) {
                            try { if (!ar[i].empty()) { long id = std::stol(ar[i]); if (id > max_id) max_id = id; } } catch(...) {}
                        }
                        last_ids[i] = max_id;
                    }
                    r[i] = std::to_string(++last_ids[i]);
                } else if (s.columns[i].has_default) {
                    r[i] = s.columns[i].default_value;
                }
            }
        }

        for (size_t i = 0; i < s.columns.size(); ++i) {
            if (s.columns[i].not_null && r[i].empty()) {
                return err("NOT NULL constraint violation: column '" + s.columns[i].name + "'");
            }
        }
        evaluated_rows.push_back(std::move(r));
    }

    // 2. Проверяем UNIQUE и PRIMARY KEY
    std::vector<Row> current_table_data; // Кэш для проверки без индексов
    bool table_data_loaded = false;

    for (size_t row_idx = 0; row_idx < evaluated_rows.size(); ++row_idx) {
        const auto& r = evaluated_rows[row_idx];
        
        for (size_t i = 0; i < s.columns.size(); ++i) {
            if (s.columns[i].unique || (int)i == s.primary_key_index) {
                const std::string& val = r[i];
                if (val.empty()) continue; // NULL значения обычно не нарушают UNIQUE (кроме PK)

                // Проверка во вставляемых данных (в рамках одного запроса)
                for (size_t prev_idx = 0; prev_idx < row_idx; ++prev_idx) {
                    if (evaluated_rows[prev_idx][i] == val) {
                        return err("UNIQUE constraint violation: duplicate value '" + val + "' in insert list");
                    }
                }

                // Проверка в существующих данных
                if ((int)i == s.primary_key_index) {
                    // Первичный ключ проверяем быстро через findRow
                    if (!storage_.findRow(current_db_, q->table_name, val).empty()) {
                        return err("PRIMARY KEY violation: duplicate key '" + val + "'");
                    }
                } else if (storage_.hasIndex(current_db_, q->table_name, s.columns[i].name)) {
                    // Вторичный индекс
                    if (!storage_.indexLookup(current_db_, q->table_name, s.columns[i].name, val).empty()) {
                        return err("UNIQUE constraint violation: duplicate value '" + val + "' in column '" + s.columns[i].name + "'");
                    }
                } else {
                    // Медленная проверка через полный скан (если индекса нет)
                    if (!table_data_loaded) {
                        current_table_data = storage_.readAllRows(current_db_, q->table_name);
                        table_data_loaded = true;
                    }
                    for (const auto& tr : current_table_data) {
                        if (tr[i] == val) {
                            return err("UNIQUE constraint violation: duplicate value '" + val + "' in column '" + s.columns[i].name + "'");
                        }
                    }
                }
            }
        }
    }

    // 3. Проверяем FOREIGN KEY
    for (const auto& r : evaluated_rows) {
        for (size_t i = 0; i < s.columns.size(); ++i) {
            if (!s.columns[i].fk_ref_table.empty()) {
                const std::string& val = r[i];
                if (val.empty()) continue; 
                auto ps = storage_.getTableSchema(current_db_, s.columns[i].fk_ref_table);
                int pi = colIndex(ps, "", s.columns[i].fk_ref_column);
                if (pi < 0) throw std::runtime_error("Invalid FK reference column");
                bool found = false;
                if (pi == ps.primary_key_index) { if (!storage_.findRow(current_db_, s.columns[i].fk_ref_table, val).empty()) found = true; }
                else if (storage_.hasIndex(current_db_, s.columns[i].fk_ref_table, s.columns[i].fk_ref_column)) {
                    if (!storage_.indexLookup(current_db_, s.columns[i].fk_ref_table, s.columns[i].fk_ref_column, val).empty()) found = true;
                } else {
                    auto prs = storage_.readAllRows(current_db_, s.columns[i].fk_ref_table);
                    for (const auto& pr : prs) if (pr[pi] == val) { found = true; break; }
                }
                if (!found) return err("FOREIGN KEY violation: value '" + val + "' not found in " + s.columns[i].fk_ref_table);
            }
        }
    }

    // 4. Если всё ок — вставляем
    int c = 0;
    for (auto& r : evaluated_rows) {
        if (storage_.appendRows(current_db_, q->table_name, {r}) > 0) {
            storage_.indexInsertRow(current_db_, q->table_name, s, r);
            c++;
        }
    }
    return ok(std::to_string(c) + " inserted.");
}
json Executor::execUpdate(const UpdateStatement* q) {
    requireDB();
    auto s = storage_.getTableSchema(current_db_, q->table_name);
    auto rows = storage_.readAllRows(current_db_, q->table_name);
    int u = 0;
    for (auto& row : rows) {
        if (q->where && !evalCondition(q->where.get(), row, s, nullptr, outer_schema_, outer_row_)) continue;
        Row old = row; Row new_row = row; bool mod = false;
        for (auto& sc : q->set_clauses) {
            int idx = colIndex(s, "", sc.column);
            if (idx >= 0) {
                new_row[idx] = evaluateExpression(sc.value.get(), row, s, nullptr, outer_schema_, outer_row_).val;
                mod = true;
            }
        }
        if (mod) {
            performUpdate(current_db_, q->table_name, s, old, new_row);
            storage_.indexRemoveRow(current_db_, q->table_name, s, old);
            storage_.indexInsertRow(current_db_, q->table_name, s, new_row);
            row = new_row; u++;
        }
    }
    if (u > 0) storage_.writeAllRows(current_db_, q->table_name, rows);
    return ok(std::to_string(u) + " updated.");
}

void Executor::performUpdate(const std::string& db_name, const std::string& table_name, const TableSchema& s, const Row& old_row, const Row& new_row) {
    auto tables = storage_.listTables(db_name);
    for (const auto& child_table_name : tables) {
        auto child_s = storage_.getTableSchema(db_name, child_table_name);
        for (const auto& child_col : child_s.columns) {
            if (child_col.fk_ref_table == table_name) {
                int parent_idx = colIndex(s, "", child_col.fk_ref_column);
                int child_idx = colIndex(child_s, "", child_col.name);
                if (parent_idx < 0 || child_idx < 0) continue;
                if (old_row[parent_idx] != new_row[parent_idx]) {
                    auto child_rows = storage_.readAllRows(db_name, child_table_name);
                    bool changed = false;
                    for (auto& cr : child_rows) {
                        if (cr[child_idx] == old_row[parent_idx]) {
                            Row old_cr = cr;
                            if (child_col.on_update == OnUpdateAction::CASCADE) cr[child_idx] = new_row[parent_idx];
                            else if (child_col.on_update == OnUpdateAction::SET_NULL) cr[child_idx] = "";
                            else throw std::runtime_error("FOREIGN KEY violation");
                            storage_.indexRemoveRow(db_name, child_table_name, child_s, old_cr);
                            storage_.indexInsertRow(db_name, child_table_name, child_s, cr);
                            performUpdate(db_name, child_table_name, child_s, old_cr, cr);
                            changed = true;
                        }
                    }
                    if (changed) storage_.writeAllRows(db_name, child_table_name, child_rows);
                }
            }
        }
    }
}
json Executor::execDelete(const DeleteStatement* q) {
    auto s = storage_.getTableSchema(current_db_, q->table_name); 
    auto rows = storage_.readAllRows(current_db_, q->table_name); 
    std::vector<Row> to_delete;
    for (auto& row : rows) {
        if (!q->where || evalCondition(q->where.get(), row, s, nullptr, outer_schema_, outer_row_)) {
            to_delete.push_back(row);
        }
    }
    
    int total_deleted = 0;
    performDelete(current_db_, q->table_name, to_delete, total_deleted);
    return ok(std::to_string(total_deleted) + " deleted.");
}

void Executor::performDelete(const std::string& db_name, const std::string& table_name, const std::vector<Row>& rows_to_delete, int& total_deleted) {
    if (rows_to_delete.empty()) return;
    auto s = storage_.getTableSchema(db_name, table_name);

    // 1. Check for child references (Cascades/Restrict)
    auto tables = storage_.listTables(db_name);
    for (const auto& child_table_name : tables) {
        if (child_table_name == table_name) continue; // Skip self for now (unless self-referencing)
        auto child_s = storage_.getTableSchema(db_name, child_table_name);
        
        for (const auto& child_col : child_s.columns) {
            if (child_col.fk_ref_table == table_name) {
                // This column references the table we are deleting from
                int parent_idx = colIndex(s, "", child_col.fk_ref_column);
                int child_idx = colIndex(child_s, "", child_col.name);
                if (parent_idx < 0 || child_idx < 0) continue;

                for (const auto& prow : rows_to_delete) {
                    const std::string& pval = prow[parent_idx];
                    
                    // Find rows in child table that point to this parent value
                    auto child_rows = storage_.readAllRows(db_name, child_table_name);
                    std::vector<Row> referencing_rows;
                    for (const auto& cr : child_rows) if (cr[child_idx] == pval) referencing_rows.push_back(cr);

                    if (referencing_rows.empty()) continue;

                    if (child_col.on_delete == OnDeleteAction::CASCADE) {
                        performDelete(db_name, child_table_name, referencing_rows, total_deleted);
                    } else if (child_col.on_delete == OnDeleteAction::SET_NULL) {
                        for (auto& cr : child_rows) {
                            if (cr[child_idx] == pval) cr[child_idx] = "";
                        }
                        storage_.writeAllRows(db_name, child_table_name, child_rows);
                    } else {
                        // NO_ACTION / RESTRICT
                        throw std::runtime_error("FOREIGN KEY violation: cannot delete row from '" + table_name + "' (referenced by '" + child_table_name + "')");
                    }
                }
            }
        }
    }

    // 2. Physical delete from current table
    auto all_rows = storage_.readAllRows(db_name, table_name);
    std::vector<Row> kept;
    for (const auto& ar : all_rows) {
        bool should_delete = false;
        for (const auto& dr : rows_to_delete) {
            if (ar[s.primary_key_index] == dr[s.primary_key_index]) { should_delete = true; break; }
        }
        if (should_delete) { storage_.indexRemoveRow(db_name, table_name, s, ar); total_deleted++; }
        else kept.push_back(ar);
    }
    storage_.writeAllRows(db_name, table_name, kept);
}

bool Executor::tryIndexScan(const SelectStatement* q, const TableSchema& s, std::vector<Row>& o) {
    if (!q->where) return false;

    // Handle BETWEEN
    if (auto bt = dynamic_cast<BetweenExpression*>(q->where.get())) {
        if (bt->negated) return false;
        auto col = dynamic_cast<ColumnExpression*>(bt->val.get());
        auto low = dynamic_cast<LiteralExpression*>(bt->low.get());
        auto high = dynamic_cast<LiteralExpression*>(bt->high.get());
        if (col && low && high && storage_.hasIndex(current_db_, q->table_name, col->column)) {
            auto pks = storage_.indexScan(current_db_, q->table_name, col->column, &low->value, &high->value);
            for (auto& pk : pks) { Row row = storage_.findRow(current_db_, q->table_name, pk); if (!row.empty()) o.push_back(row); }
            return true;
        }
        return false;
    }

    // Handle Binary Operations (=, <, <=, >, >=)
    auto b = dynamic_cast<BinaryExpression*>(q->where.get()); 
    if (!b) return false;

    ColumnExpression* col = nullptr;
    LiteralExpression* lit = nullptr;
    bool swap = false;

    if ((col = dynamic_cast<ColumnExpression*>(b->left.get())) && (lit = dynamic_cast<LiteralExpression*>(b->right.get()))) {
        // col op lit
    } else if ((lit = dynamic_cast<LiteralExpression*>(b->left.get())) && (col = dynamic_cast<ColumnExpression*>(b->right.get()))) {
        // lit op col -> swap to col op lit
        swap = true;
    }

    if (!col || !lit || !storage_.hasIndex(current_db_, q->table_name, col->column)) return false;

    TokenType op = b->op;
    if (swap) {
        if (op == TokenType::OP_LT) op = TokenType::OP_GT;
        else if (op == TokenType::OP_GT) op = TokenType::OP_LT;
        else if (op == TokenType::OP_LTE) op = TokenType::OP_GTE;
        else if (op == TokenType::OP_GTE) op = TokenType::OP_LTE;
    }

    std::vector<std::string> pks;
    if (op == TokenType::OP_EQ) {
        pks = storage_.indexLookup(current_db_, q->table_name, col->column, lit->value);
    } else if (op == TokenType::OP_GT || op == TokenType::OP_GTE) {
        // Value in index is strictly greater than lit->value or >=
        // For simplicity, we use lit->value as low bound and filter in BPlusTree or here.
        // Storage::indexScan(low, high) is [low, high] inclusive.
        // So for >, we might need to skip the exact match if it exists.
        pks = storage_.indexScan(current_db_, q->table_name, col->column, &lit->value, nullptr);
        if (op == TokenType::OP_GT && !pks.empty()) {
            // This is a bit inefficient because we might fetch the first row just to skip it.
            // But indexScan results are already sorted.
            // Better: storage handles it, but let's just do a quick fix.
            // Actually, storage_.indexScan currently returns all matches in [low, high].
        }
    } else if (op == TokenType::OP_LT || op == TokenType::OP_LTE) {
        pks = storage_.indexScan(current_db_, q->table_name, col->column, nullptr, &lit->value);
    } else {
        return false;
    }

    if (pks.empty()) return true; // Return true as we used the index (even if no matches found)

    for (auto& pk : pks) { 
        Row row = storage_.findRow(current_db_, q->table_name, pk); 
        if (row.empty()) continue;
        
        // Final filter for strict inequalities if needed
        if (op == TokenType::OP_GT || op == TokenType::OP_LT) {
            Value v = {row[colIndex(s, "", col->column)], ""};
            Value lv = {lit->value, ""};
            int cmp = compareValues(v, lv);
            if (op == TokenType::OP_GT && cmp <= 0) continue;
            if (op == TokenType::OP_LT && cmp >= 0) continue;
        }
        o.push_back(std::move(row)); 
    }
    return true;
}

json Executor::execSelect(const SelectStatement* q) {
    TableSchema m;
    std::vector<Row> rows;
    bool sorted_by_index = false;
    std::string effective_root_table;

    if (q->from_subquery) {
        json sub_res = execSelect(q->from_subquery.get());
        if (!sub_res["success"]) throw std::runtime_error("Subquery failed");
        
        effective_root_table = q->from_alias;
        m.table_name = effective_root_table;
        for (auto& col_name : sub_res["columns"]) {
            ColumnDef cd;
            cd.name = effective_root_table + "." + col_name.get<std::string>();
            cd.type = "TEXT";
            m.columns.push_back(cd);
        }
        for (auto& r_json : sub_res["rows"]) {
            Row r;
            for (auto& val : r_json) r.push_back(val.get<std::string>());
            rows.push_back(r);
        }
    } else {
        requireDB();
        auto s = storage_.getTableSchema(current_db_, q->table_name);
        if (!tryIndexScan(q, s, rows)) {
            if (!q->where && q->order_by.size() == 1 && !q->distinct && q->group_by.empty()) {
                auto ce = dynamic_cast<ColumnExpression*>(q->order_by[0].expr.get());
                if (ce && storage_.hasIndex(current_db_, q->table_name, ce->column)) {
                    auto pks = storage_.indexScan(current_db_, q->table_name, ce->column, nullptr, nullptr);
                    if (!q->order_by[0].asc) std::reverse(pks.begin(), pks.end());
                    for (auto& pk : pks) {
                        Row r = storage_.findRow(current_db_, q->table_name, pk);
                        if (!r.empty()) rows.push_back(std::move(r));
                    }
                    sorted_by_index = true;
                }
            }
            if (rows.empty() && !sorted_by_index) rows = storage_.readAllRows(current_db_, q->table_name);
        }
        
        m = s;
        effective_root_table = q->alias.empty() ? q->table_name : q->alias;
        m.table_name = effective_root_table;
        for (auto& c : m.columns) c.name = effective_root_table + "." + c.name;
    }
    for (auto& jc : q->joins) {
        auto rs = storage_.getTableSchema(current_db_, jc.table_name);
        std::string rs_effective_name = jc.alias.empty() ? jc.table_name : jc.alias;
        rs.table_name = rs_effective_name; // Set effective name for colIndex resolution

        std::vector<Row> res;

        int li = -1, ri = -1;
        if (jc.join_type != JoinClause::CROSS) {
            li = colIndex(m, jc.left_col.table, jc.left_col.column);
            ri = colIndex(rs, jc.right_col.table.empty() ? rs_effective_name : jc.right_col.table, jc.right_col.column);
            if (li < 0 || ri < 0) throw std::runtime_error("Invalid JOIN column(s)");
        }

        bool is_pk = false;
        bool has_sec_idx = false;
        if (jc.join_type != JoinClause::CROSS) {
            is_pk = (ri == rs.primary_key_index);
            has_sec_idx = storage_.hasIndex(current_db_, jc.table_name, rs.columns[ri].name);
        }
        bool use_index_join = (is_pk || has_sec_idx) && (jc.join_type == JoinClause::INNER || jc.join_type == JoinClause::LEFT);

        constexpr bool USE_HASH_JOIN = true;

        if (jc.join_type == JoinClause::CROSS) {
            auto rr = storage_.readAllRows(current_db_, jc.table_name);
            for (auto& lr : rows) {
                for (size_t i = 0; i < rr.size(); ++i) {
                    Row c = lr; c.insert(c.end(), rr[i].begin(), rr[i].end()); res.push_back(std::move(c));
                }
            }
        } else if (use_index_join) {
            for (auto& lr : rows) {
                std::string key = lr[li];
                bool matched = false;

                if (is_pk) {
                    Row rr = storage_.findRow(current_db_, jc.table_name, key);
                    if (!rr.empty()) {
                        Row c = lr; c.insert(c.end(), rr.begin(), rr.end()); res.push_back(std::move(c));
                        matched = true;
                    }
                } else {
                    auto pks = storage_.indexLookup(current_db_, jc.table_name, rs.columns[ri].name, key);
                    for (const auto& pk : pks) {
                        Row rr = storage_.findRow(current_db_, jc.table_name, pk);
                        if (!rr.empty()) {
                            Row c = lr; c.insert(c.end(), rr.begin(), rr.end()); res.push_back(std::move(c));
                            matched = true;
                        }
                    }
                }

                if (!matched && jc.join_type == JoinClause::LEFT) {
                    Row c = lr; c.insert(c.end(), rs.columns.size(), ""); res.push_back(std::move(c));
                }
            }
        } else if (USE_HASH_JOIN) {
            // Hash Join implementation for all remaining JOIN types (INNER, LEFT, RIGHT, FULL)
            auto rr = storage_.readAllRows(current_db_, jc.table_name);
            std::unordered_map<std::string, std::vector<size_t>> hash_map;
            for (size_t i = 0; i < rows.size(); ++i) {
                hash_map[rows[i][li]].push_back(i);
            }
            std::vector<bool> left_matched(rows.size(), false);
            std::vector<bool> right_matched(rr.size(), false);

            for (size_t i = 0; i < rr.size(); ++i) {
                std::string r_key = rr[i][ri];
                auto it = hash_map.find(r_key);
                if (it != hash_map.end()) {
                    for (size_t l_idx : it->second) {
                        Row c = rows[l_idx]; c.insert(c.end(), rr[i].begin(), rr[i].end());
                        res.push_back(std::move(c));
                        left_matched[l_idx] = true;
                        right_matched[i] = true;
                    }
                }
            }

            if (jc.join_type == JoinClause::LEFT || jc.join_type == JoinClause::FULL) {
                for (size_t i = 0; i < rows.size(); ++i) {
                    if (!left_matched[i]) {
                        Row c = rows[i]; c.insert(c.end(), rs.columns.size(), ""); res.push_back(std::move(c));
                    }
                }
            }
            if (jc.join_type == JoinClause::RIGHT || jc.join_type == JoinClause::FULL) {
                for (size_t i = 0; i < rr.size(); ++i) {
                    if (!right_matched[i]) {
                        Row c(m.columns.size(), ""); c.insert(c.end(), rr[i].begin(), rr[i].end()); res.push_back(std::move(c));
                    }
                }
            }
        } else {
            // Classic Nested Loop Join (kept as a disabled fallback)
            auto rr = storage_.readAllRows(current_db_, jc.table_name);
            std::vector<bool> rm(rr.size(), false);
            for (auto& lr : rows) {
                bool matched = false;
                for (size_t i = 0; i < rr.size(); ++i) {
                    if (lr[li] == rr[i][ri]) {
                        Row c = lr; c.insert(c.end(), rr[i].begin(), rr[i].end());
                        res.push_back(std::move(c)); matched = true; rm[i] = true;
                    }
                }
                if (!matched && (jc.join_type == JoinClause::LEFT || jc.join_type == JoinClause::FULL)) {
                    Row c = lr; c.insert(c.end(), rs.columns.size(), ""); res.push_back(std::move(c));
                }
            }
            if (jc.join_type == JoinClause::RIGHT || jc.join_type == JoinClause::FULL) {
                for (size_t i = 0; i < rr.size(); ++i) {
                    if (!rm[i]) {
                        Row c(m.columns.size(), ""); c.insert(c.end(), rr[i].begin(), rr[i].end());
                        res.push_back(std::move(c));
                    }
                }
            }
        }

        for (auto& c : rs.columns) { 
            ColumnDef cd = c; 
            std::string effective_join_table = jc.alias.empty() ? jc.table_name : jc.alias;
            cd.name = effective_join_table + "." + c.name; 
            m.columns.push_back(std::move(cd)); 
        }
        rows = std::move(res);
    }
    if (q->where) { std::vector<Row> f; for (auto& r : rows) if (evalCondition(q->where.get(), r, m, nullptr, outer_schema_, outer_row_)) f.push_back(r); rows = std::move(f); }

    struct Group { Row rep; std::map<std::pair<AggrFunc, std::string>, AggrState> st; };
    std::vector<Group> groups; bool is_aggr = !q->group_by.empty();
    std::function<void(const Expression*)> check_aggr = [&](const Expression* e) {
        if (!e) return;
        if (dynamic_cast<const AggregateExpression*>(e)) is_aggr = true;
        else if (auto b = dynamic_cast<const BinaryExpression*>(e)) { check_aggr(b->left.get()); check_aggr(b->right.get()); }
        else if (auto u = dynamic_cast<const UnaryExpression*>(e)) check_aggr(u->operand.get());
        else if (auto in_list = dynamic_cast<const InListExpression*>(e)) check_aggr(in_list->left.get());
    };
    for (auto& sc : q->select_columns) check_aggr(sc.expr.get());
    check_aggr(q->having.get());
    for (auto& ob : q->order_by) check_aggr(ob.expr.get());

    if (is_aggr) {
        std::set<std::pair<std::pair<AggrFunc, std::string>, bool>> unique_aggr_exprs;
        std::function<void(const Expression*)> find_aggrs = [&](const Expression* e) {
            if (!e) return;
            if (auto a = dynamic_cast<const AggregateExpression*>(e)) unique_aggr_exprs.insert({{a->func, a->column}, a->distinct});
            else if (auto b = dynamic_cast<const BinaryExpression*>(e)) { find_aggrs(b->left.get()); find_aggrs(b->right.get()); }
            else if (auto u = dynamic_cast<const UnaryExpression*>(e)) find_aggrs(u->operand.get());
            else if (auto in_list = dynamic_cast<const InListExpression*>(e)) find_aggrs(in_list->left.get());
        };
        for (auto& sc : q->select_columns) find_aggrs(sc.expr.get());
        find_aggrs(q->having.get());
        for (auto& ob : q->order_by) find_aggrs(ob.expr.get());

        std::map<std::string, Group> g_map;
        for (auto& row : rows) {
            std::string k; for (auto& gb : q->group_by) { int i = colIndex(m, "", gb); k += (i >= 0 ? row[i] : "") + "|"; }
            auto& g = g_map[k]; if (g.rep.empty()) g.rep = row;
            for (auto& aggr_pair : unique_aggr_exprs) {
                auto& aggr = aggr_pair.first;
                bool is_distinct = aggr_pair.second;
                auto& st = g.st[aggr];
                
                std::string val_to_add;
                if (aggr.second == "*") val_to_add = "*";
                else {
                    int i = colIndex(m, "", aggr.second);
                    if (i >= 0) val_to_add = row[i];
                }

                if (is_distinct) {
                    if (st.seen_values.find(val_to_add) != st.seen_values.end()) continue;
                    st.seen_values.insert(val_to_add);
                }

                st.count++; 
                if (val_to_add == "*") continue;
                if (!val_to_add.empty()) {
                    try {
                        double v = std::stod(val_to_add);
                        if (!st.initialized) { st.sum = st.min_val = st.max_val = v; st.initialized = true; }
                        else { st.sum += v; st.min_val = std::min(st.min_val, v); st.max_val = std::max(st.max_val, v); }
                    } catch(...) {}
                }
            }
        }
        if (g_map.empty() && q->group_by.empty()) {
            Group& g = g_map[""];
            g.rep = Row(m.columns.size(), "");
            for (auto& aggr_pair : unique_aggr_exprs) {
                auto& st = g.st[aggr_pair.first];
                st.count = 0;
                st.sum = 0;
                st.initialized = false;
            }
        }
        for (auto& kv : g_map) if (!q->having || evalCondition(q->having.get(), kv.second.rep, m, &kv.second.st, outer_schema_, outer_row_)) groups.push_back(std::move(kv.second));
    } else {
        for (auto& r : rows) { Group g; g.rep = r; groups.push_back(std::move(g)); }
    }

    if (!q->order_by.empty() && !sorted_by_index) {
        std::sort(groups.begin(), groups.end(), [&](const Group& a, const Group& b) {
            for (auto& ob : q->order_by) {
                Value va = evaluateExpression(ob.expr.get(), a.rep, m, &a.st, outer_schema_, outer_row_);
                Value vb = evaluateExpression(ob.expr.get(), b.rep, m, &b.st, outer_schema_, outer_row_);
                int cmp = compareValues(va, vb);
                if (cmp != 0) return ob.asc ? cmp < 0 : cmp > 0;
            }
            return false;
        });
    }

    std::vector<Row> res_r; std::vector<std::string> cn;
    if (q->select_all) for (auto& c : m.columns) cn.push_back(c.name);
    else for (auto& sc : q->select_columns) {
        if (!sc.alias.empty()) cn.push_back(sc.alias);
        else if (auto ce = dynamic_cast<ColumnExpression*>(sc.expr.get())) cn.push_back(ce->column);
        else if (dynamic_cast<AggregateExpression*>(sc.expr.get())) cn.push_back("aggr");
        else cn.push_back("expr");
    }

    for (auto& g : groups) {
        Row r; if (q->select_all) r = g.rep;
        else for (auto& sc : q->select_columns) {
            r.push_back(evaluateExpression(sc.expr.get(), g.rep, m, &g.st, outer_schema_, outer_row_).val);
        }
        res_r.push_back(std::move(r));
    }

    if (q->distinct) {
        std::set<Row> unique_rows;
        std::vector<Row> distinct_rows;
        for (auto& r : res_r) {
            if (unique_rows.find(r) == unique_rows.end()) {
                unique_rows.insert(r);
                distinct_rows.push_back(std::move(r));
            }
        }
        res_r = std::move(distinct_rows);
    }

    json res; res["success"] = true; res["type"] = "select"; res["columns"] = cn; res["rows"] = json::array();
    int s_i = std::max(0, q->offset), count = q->limit >= 0 ? q->limit : (int)res_r.size(), e_i = std::min((int)res_r.size(), s_i + count);
    for (int i = s_i; i < e_i; ++i) { json jr = json::array(); for (auto& v : res_r[i]) jr.push_back(v); res["rows"].push_back(jr); }
    return res;
}

std::vector<Row> Executor::execute_subquery(const SelectStatement* q, const TableSchema* os, const Row* orow) {
    const TableSchema* ps = outer_schema_; const Row* pr = outer_row_; outer_schema_ = os; outer_row_ = orow;
    auto r = execSelect(q); outer_schema_ = ps; outer_row_ = pr; std::vector<Row> rws;
    if (r["success"]) for (auto& rr : r["rows"]) { Row row; for (auto& v : rr) row.push_back(v.get<std::string>()); rws.push_back(row); }
    return rws;
}

} // namespace db
