#include "engine/executor.hpp"
#include "engine/cell_value.hpp"
#include "engine/crypto.hpp"
#include <cctype>
#include <stdexcept>
#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <unordered_map>
#include <iomanip>
#include <sstream>

#include <regex>
#include <iostream>
#include <fstream>
#include <filesystem>

namespace db {

namespace {

ColumnDef column_def_from_col_def(const ColDef& c) {
    ColumnDef d;
    d.name = c.name;
    d.type = c.type;
    d.not_null = c.not_null;
    d.unique = c.unique;
    d.has_default = c.has_default;
    d.is_autoincrement = c.is_autoincrement;
    d.default_value = c.default_value;
    d.fk_ref_table = c.fk_ref_table;
    d.fk_ref_column = c.fk_ref_column;
    d.on_delete = c.on_delete;
    d.on_update = c.on_update;
    return d;
}

CellValue value_to_cell(const ColumnDef& col, const Value& v) {
    if (v.type == "NULL")
        return std::nullopt;
    if (col.type == "TEXT")
        return CellPrimitive{v.val};
    if (col.type == "BOOL") {
        std::string s = v.val;
        for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (s == "1" || s == "true")
            return CellPrimitive{true};
        if (s == "0" || s == "false")
            return CellPrimitive{false};
        return CellPrimitive{v.val};
    }
    try {
        if (col.type == "INT")
            return static_cast<int64_t>(std::stoll(v.val));
        if (col.type == "FLOAT")
            return std::stod(v.val);
    } catch (...) {
    }
    return coerce_string_to_cell_column(col, v.val, false);
}

} // namespace

static std::string trim_csv_field(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
        s.erase(s.begin());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.pop_back();
    return s;
}

static bool column_type_is_text_like(const ColumnDef& col) {
    if (col.type == "TEXT")
        return true;
    return col.type.size() >= 7 && col.type.compare(0, 7, "VARCHAR") == 0;
}

static std::vector<std::string> split_csv_line(const std::string& line) {
    std::vector<std::string> fields;
    std::string cur;
    bool in_quotes = false;
    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (in_quotes) {
            if (c == '"') {
                if (i + 1 < line.size() && line[i + 1] == '"') {
                    cur += '"';
                    ++i;
                } else {
                    in_quotes = false;
                }
            } else {
                cur += c;
            }
        } else {
            if (c == '"')
                in_quotes = true;
            else if (c == ',') {
                fields.push_back(cur);
                cur.clear();
            } else {
                cur += c;
            }
        }
    }
    fields.push_back(cur);
    return fields;
}

static CellValue cell_from_csv_field(const ColumnDef& col, const std::string& raw) {
    std::string s = trim_csv_field(raw);
    if (s.empty()) {
        if (column_type_is_text_like(col))
            return CellPrimitive{std::string{}};
        return std::nullopt;
    }
    return coerce_string_to_cell_column(col, s, column_type_is_text_like(col));
}

static std::filesystem::path resolve_csv_path(const Storage& storage, const std::string& db_name,
                                              const std::string& literal_path) {
    std::filesystem::path p(literal_path);
    if (p.is_absolute())
        return p;
    return storage.databaseDirectory(db_name) / p;
}

static std::string strip_utf8_bom(std::string line) {
    if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF &&
        static_cast<unsigned char>(line[1]) == 0xBB && static_cast<unsigned char>(line[2]) == 0xBF)
        return line.substr(3);
    return line;
}

static std::string strip_cr(std::string line) {
    if (!line.empty() && line.back() == '\r')
        line.pop_back();
    return line;
}

static std::string read_file_all(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw std::runtime_error("Cannot open file: " + path.string());
    std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return data;
}

static std::vector<std::string> split_lines_unix(const std::string& data) {
    std::vector<std::string> lines;
    size_t start = 0;
    for (size_t i = 0; i < data.size(); ++i) {
        if (data[i] == '\n') {
            lines.push_back(data.substr(start, i - start));
            start = i + 1;
        }
    }
    if (start < data.size())
        lines.push_back(data.substr(start));
    else if (!data.empty() && data.back() == '\n')
        lines.push_back("");
    return lines;
}

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

Executor::Executor(const std::string& data_dir) : storage_(data_dir) {
    // Гарантируем, что существует хотя бы одна БД для первичной авторизации
    if (!storage_.databaseExists("system")) {
        storage_.createDatabase("system");
    }
}

json Executor::ok(const std::string& msg) {
    return {{"success", true}, {"message", msg}, {"current_db", current_db_}, {"current_user", current_user_}};
}

json Executor::err(const std::string& msg) {
    return {{"success", false}, {"message", msg}, {"current_db", current_db_}, {"current_user", current_user_}};
}

void Executor::requireDB() const {
    if (current_user_.empty()) {
        throw std::runtime_error("Not authenticated. Use LOGIN or REGISTER first.");
    }
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
                                   const std::vector<SelectColumn>* select_cols,
                                   const TableSchema* outer_schema, const Row* outer_row) {
    if (!expr) return {"", "NULL"};
    if (auto lit = dynamic_cast<const LiteralExpression*>(expr)) {
        if (lit->type == TokenType::NUMBER_LITERAL) return {lit->value, lit->value.find('.') != std::string::npos ? "FLOAT" : "INT"};
        if (lit->type == TokenType::BOOL_LITERAL) {
            std::string lv = lit->value;
            for (auto& ch : lv) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            bool on = (lv == "true" || lv == "1");
            return {on ? "true" : "false", "BOOL"};
        }
        if (lit->type == TokenType::KW_NULL) return {"", "NULL"};
        if (lit->type == TokenType::KW_CURRENT_DATE) {
            auto t = std::time(nullptr);
            auto tm = *std::localtime(&t);
            std::ostringstream oss;
            oss << std::put_time(&tm, "%Y-%m-%d");
            return {oss.str(), "TEXT"};
        }
        return {lit->value, "TEXT"};
    }
    if (auto col = dynamic_cast<const ColumnExpression*>(expr)) {
        int idx = colIndex(schema, col->table, col->column);
        if (idx >= 0) {
            CellValue cv =
                (idx < (int)row.size()) ? row[static_cast<size_t>(idx)] : CellValue{};
            if (!cv.has_value())
                return {"", "NULL"};
            return {cell_to_where_string(cv), schema.columns[static_cast<size_t>(idx)].type};
        }
        if (select_cols) {
            for (auto& sc : *select_cols) {
                if (sc.alias == col->column) {
                    return evaluateExpression(sc.expr.get(), row, schema, aggrs, nullptr, outer_schema, outer_row);
                }
            }
        }
        if (outer_schema && outer_row) {
            idx = colIndex(*outer_schema, col->table, col->column);
            if (idx >= 0) {
                CellValue cv = (idx < (int)outer_row->size())
                                   ? (*outer_row)[static_cast<size_t>(idx)]
                                   : CellValue{};
                if (!cv.has_value())
                    return {"", "NULL"};
                return {cell_to_where_string(cv), outer_schema->columns[static_cast<size_t>(idx)].type};
            }
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
        if (un->op == TokenType::KW_NOT) return {evalCondition(un->operand.get(), row, schema, aggrs, select_cols, outer_schema, outer_row) ? "0" : "1", "BOOL"};
        if (un->op == TokenType::OP_MINUS) {
            Value v = evaluateExpression(un->operand.get(), row, schema, aggrs, select_cols, outer_schema, outer_row);
            double d = v.val.empty() ? 0 : std::stod(v.val);
            return {formatFloat(-d), "FLOAT"};
        }
    }
    if (auto bin = dynamic_cast<const BinaryExpression*>(expr)) {
        if (bin->op == TokenType::KW_AND) {
            if (!evalCondition(bin->left.get(), row, schema, aggrs, select_cols, outer_schema, outer_row)) return {"0", "BOOL"};
            return {evalCondition(bin->right.get(), row, schema, aggrs, select_cols, outer_schema, outer_row) ? "1" : "0", "BOOL"};
        }
        if (bin->op == TokenType::KW_OR) {
            if (evalCondition(bin->left.get(), row, schema, aggrs, select_cols, outer_schema, outer_row)) return {"1", "BOOL"};
            return {evalCondition(bin->right.get(), row, schema, aggrs, select_cols, outer_schema, outer_row) ? "1" : "0", "BOOL"};
        }
        Value left = evaluateExpression(bin->left.get(), row, schema, aggrs, select_cols, outer_schema, outer_row);
        if (bin->op == TokenType::KW_IN) {
            if (auto in_sub =
                    dynamic_cast<const SubqueryExpression*>(bin->right.get())) {
                auto rows_in = const_cast<Executor*>(this)->execute_subquery(
                    in_sub->subquery.get(), &schema, &row);
                bool found = false;
                for (const auto& one : rows_in) {
                    if (!one.empty() && cell_to_where_string(one[0]) == left.val) {
                        found = true;
                        break;
                    }
                }
                return {(in_sub->negated ? !found : found) ? "1" : "0", "BOOL"};
            }
        }
        Value right = evaluateExpression(bin->right.get(), row, schema, aggrs, select_cols, outer_schema, outer_row);
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
        if (left.type == "NULL" || right.type == "NULL") {
            return {"0", "BOOL"};
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
        Value left = evaluateExpression(in_list->left.get(), row, schema, aggrs, select_cols, outer_schema, outer_row);
        bool found = std::find(in_list->values.begin(), in_list->values.end(), left.val) != in_list->values.end();
        return {(in_list->negated ? !found : found) ? "1" : "0", "BOOL"};
    }
    if (auto is_null = dynamic_cast<const IsNullExpression*>(expr)) {
        Value v = evaluateExpression(is_null->operand.get(), row, schema, aggrs, select_cols, outer_schema, outer_row);
        bool is_v_null = (v.type == "NULL");
        return {(is_null->is_not ? !is_v_null : is_v_null) ? "1" : "0", "BOOL"};
    }
    if (auto lk = dynamic_cast<const LikeExpression*>(expr)) {
        Value v = evaluateExpression(lk->left.get(), row, schema, aggrs, select_cols, outer_schema, outer_row);
        bool res = likeMatch(v.val, lk->pattern);
        return {(lk->negated ? !res : res) ? "1" : "0", "BOOL"};
    }
    if (auto bt = dynamic_cast<const BetweenExpression*>(expr)) {
        Value v = evaluateExpression(bt->val.get(), row, schema, aggrs, select_cols, outer_schema, outer_row);
        Value low = evaluateExpression(bt->low.get(), row, schema, aggrs, select_cols, outer_schema, outer_row);
        Value high = evaluateExpression(bt->high.get(), row, schema, aggrs, select_cols, outer_schema, outer_row);
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
        return {cell_to_where_string(res[0][0]), "TEXT"};
    }
    return {"", "NULL"};
}

bool Executor::evalCondition(const Expression* expr, const Row& row, const TableSchema& schema,
                             const std::map<std::pair<AggrFunc, std::string>, AggrState>* aggrs,
                             const std::vector<SelectColumn>* select_cols,
                             const TableSchema* outer_schema, const Row* outer_row) {
    if (!expr) return true;
    return evaluateExpression(expr, row, schema, aggrs, select_cols, outer_schema, outer_row).val == "1";
}

void Executor::checkPermission(const std::string& table_name, const std::string& privilege) {
    if (current_user_.empty()) {
        throw std::runtime_error("Not authenticated. Use LOGIN or REGISTER first.");
    }

    if (current_db_.empty()) {
        throw std::runtime_error("No database selected. Use: USE <database>;");
    }

    if (!storage_.checkPrivilege(current_db_, current_user_, table_name, privilege)) {
        throw std::runtime_error("Permission denied: " + privilege + " on " + table_name +
                                 " in database '" + current_db_ + "'.");
    }
}

json Executor::execute(const std::string& sql) {
    try {
        Lexer l(sql); auto tokens = l.tokenize();

        // Database and auth checks are now handled inside the individual exec* methods

        Parser p(tokens); auto query = p.parse();
        if (dynamic_cast<BeginStatement*>(query.get())) return execBegin();
        if (dynamic_cast<CommitStatement*>(query.get())) return execCommit();
        if (dynamic_cast<RollbackStatement*>(query.get())) return execRollback();

        if (storage_.transactionActive() &&
            !dynamic_cast<SelectStatement*>(query.get()) &&
            !dynamic_cast<InsertStatement*>(query.get()) &&
            !dynamic_cast<UpdateStatement*>(query.get()) &&
            !dynamic_cast<DeleteStatement*>(query.get())) {
            return err("Only SELECT/INSERT/UPDATE/DELETE are supported inside a transaction");
        }

        if (auto q = dynamic_cast<RegisterStatement*>(query.get())) return execRegister(q);
        if (auto q = dynamic_cast<LoginStatement*>(query.get())) return execLogin(q);
        if (auto q = dynamic_cast<ChangePasswordStatement*>(query.get())) return execChangePassword(q);
        if (auto q = dynamic_cast<LogoutStatement*>(query.get())) return execLogout();

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
        if (auto q = dynamic_cast<LoadCsvStatement*>(query.get())) return execLoadCsv(q);
        if (auto q = dynamic_cast<UpdateStatement*>(query.get())) return execUpdate(q);
        if (auto q = dynamic_cast<DeleteStatement*>(query.get())) return execDelete(q);
        if (auto q = dynamic_cast<ShowStatement*>(query.get())) return execShow(q);

        if (auto q = dynamic_cast<CreateUserStatement*>(query.get())) return execCreateUser(q);
        if (auto q = dynamic_cast<CreateRoleStatement*>(query.get())) return execCreateRole(q);
        if (auto q = dynamic_cast<SetUserStatement*>(query.get())) return execSetUser(q);
        if (auto q = dynamic_cast<GrantRoleStatement*>(query.get())) return execGrantRole(q);
        if (auto q = dynamic_cast<GrantStatement*>(query.get())) return execGrant(q);
        if (auto q = dynamic_cast<GrantDdlStatement*>(query.get())) return execGrantDdl(q);
        if (auto q = dynamic_cast<RevokeDdlStatement*>(query.get())) return execRevokeDdl(q);
        return err("Unknown query type");
    } catch (const std::exception& e) { return err(e.what()); }
}

json Executor::execBegin() {
    storage_.beginTransaction();
    return ok("Transaction started.");
}

json Executor::execCommit() {
    storage_.commitTransaction();
    return ok("Transaction committed.");
}

json Executor::execRollback() {
    storage_.rollbackTransaction();
    return ok("Transaction rolled back.");
}

json Executor::execCreateDB(const CreateDatabaseStatement* q) {
    if (current_user_.empty()) return err("Not authenticated.");
    if (storage_.databaseExists(q->database_name)) return err("Database '" + q->database_name + "' already exists.");
    if (q->database_name == "system") return err("Cannot create reserved database 'system'.");
    if (!storage_.createDatabase(q->database_name)) return err("Failed to create database.");

    // Записываем владельца в system.sys_db_owners
    auto sch = storage_.getTableSchema("system", "sys_db_owners");
    Row row(sch.columns.size());
    row[1] = coerce_string_to_cell_column(sch.columns[1], q->database_name, false);
    row[2] = coerce_string_to_cell_column(sch.columns[2], current_user_, false);
    std::map<int, long> dummy;
    applyDefaultsAndAutoincrement("system", sch, "sys_db_owners", row, dummy);
    storage_.appendRows("system", "sys_db_owners", {row});
    storage_.indexInsertRow("system", "sys_db_owners", sch, row);

    return ok("Database '" + q->database_name + "' created (owner: " + current_user_ + ").");
}

json Executor::execUse(const UseDatabaseStatement* q) {
    if (q->database_name == "system" && !isAdmin()) {
        return err("Permission denied: only admin can use the 'system' database.");
    }
    if (storage_.databaseExists(q->database_name)) {
        current_db_ = q->database_name;
        json res = ok("Using database '" + q->database_name + "'.");
        res["current_db"] = current_db_; // Сообщаем клиенту об успешной смене
        return res;
    }
    return err("Database '" + q->database_name + "' does not exist.");
}

json Executor::execDropDB(const DropDatabaseStatement* q) {
    if (current_user_.empty()) return err("Not authenticated.");
    if (q->database_name == "system") return err("Cannot drop system database.");
    if (!storage_.databaseExists(q->database_name)) {
        if (q->if_exists) return ok("Dropped database '" + q->database_name + "' (if existed).");
        return err("Database '" + q->database_name + "' does not exist.");
    }
    // Только владелец или админ может удалить БД
    std::string owner = storage_.getDbOwner(q->database_name);
    if (!isAdmin() && owner != current_user_) {
        return err("Permission denied: only owner '" + owner + "' or admin can drop this database.");
    }
    if (storage_.dropDatabase(q->database_name)) {
        json res = ok("Dropped database '" + q->database_name + "'.");
        if (current_db_ == q->database_name) {
            current_db_.clear();
            res["current_db"] = "";
        }
        return res;
    }
    return err("Failed to drop database.");
}
json Executor::execCreateTable(const CreateTableStatement* q) {
    requireDB();
    checkPermission(q->table_name, "CREATE");

    TableSchema s; s.table_name = q->table_name;
    for (auto& cd : q->column_defs) { ColumnDef c; c.name = cd.name; c.type = cd.type; c.not_null = cd.not_null; c.unique = cd.unique; c.has_default = cd.has_default; c.is_autoincrement = cd.is_autoincrement; c.default_value = cd.default_value; c.fk_ref_table = cd.fk_ref_table; c.fk_ref_column = cd.fk_ref_column; c.on_delete = cd.on_delete; c.on_update = cd.on_update; s.columns.push_back(c); }
    s.primary_key_index = q->primary_key_index >= 0 ? q->primary_key_index : 0;
    if (storage_.createTable(current_db_, s)) return ok("Table created.");
    throw std::runtime_error("Table '" + q->table_name + "' already exists");
}
json Executor::execDropTable(const DropTableStatement* q) {
    requireDB();
    checkPermission(q->table_name, "DROP");

    if (storage_.dropTable(current_db_, q->table_name)) return ok("Dropped.");
    if (q->if_exists) return ok("Dropped.");
    throw std::runtime_error("Table '" + q->table_name + "' does not exist");
}
json Executor::execAlterTable(const AlterTableStatement* q) {
    requireDB();
    checkPermission(q->table_name, "ALTER");

    if (q->alter_action == AlterAction::ADD_COL) {
        if (!q->add_column_csv_path.empty())
            return execAlterTableAddColumnFromCsv(q);
        if (storage_.alterTableAddColumn(current_db_, q->table_name, column_def_from_col_def(q->alter_col_def)))
            return ok("Added.");
        return err("Failed.");
    }
    if (storage_.alterTableDropColumn(current_db_, q->table_name, q->alter_col_name))
        return ok("Dropped.");
    return err("Failed.");
}

json Executor::execAlterTableAddColumnFromCsv(const AlterTableStatement* q) {
    const ColumnDef new_cd = column_def_from_col_def(q->alter_col_def);
    if (new_cd.is_autoincrement)
        return err("ALTER TABLE ADD COLUMN ... FROM CSV: AUTOINCREMENT is not supported for this form.");

    auto s_before = storage_.getTableSchema(current_db_, q->table_name);
    const int pk_idx = s_before.primary_key_index;
    if (pk_idx < 0 || pk_idx >= static_cast<int>(s_before.columns.size()))
        return err("ALTER TABLE ADD COLUMN FROM CSV: invalid primary key index.");

    for (const auto& col : s_before.columns) {
        if (col.name == new_cd.name)
            return err("Column already exists: " + new_cd.name);
    }

    const std::string& pk_name = s_before.columns[static_cast<size_t>(pk_idx)].name;

    std::filesystem::path fpath = resolve_csv_path(storage_, current_db_, q->add_column_csv_path);
    std::string raw;
    try {
        raw = read_file_all(fpath);
    } catch (const std::exception& e) {
        return err(e.what());
    }
    if (raw.empty())
        return err("CSV file is empty");

    std::vector<std::string> lines = split_lines_unix(raw);
    if (lines.size() < 2)
        return err("ALTER TABLE ADD COLUMN FROM CSV: need a header row and at least one data row");

    std::string header_line = strip_cr(strip_utf8_bom(lines[0]));
    std::vector<std::string> header_fields = split_csv_line(header_line);
    if (header_fields.size() != 2)
        return err("ALTER TABLE ADD COLUMN FROM CSV: header must contain exactly two columns: "
                   "primary key '" +
                   pk_name + "' and new column '" + new_cd.name + "'");

    std::string h0 = trim_csv_field(header_fields[0]);
    std::string h1 = trim_csv_field(header_fields[1]);
    if ((h0 == pk_name && h1 == new_cd.name) || (h1 == pk_name && h0 == new_cd.name)) {
        // ok
    } else {
        return err("ALTER TABLE ADD COLUMN FROM CSV: header must be exactly '" + pk_name + "' and '" +
                   new_cd.name + "'");
    }
    const int pk_csv_idx = (h0 == pk_name) ? 0 : 1;
    const int new_csv_idx = 1 - pk_csv_idx;

    std::map<std::string, std::string> pk_to_new_raw;
    for (size_t li = 1; li < lines.size(); ++li) {
        std::string line = strip_cr(lines[li]);
        if (trim_csv_field(line).empty())
            continue;

        std::vector<std::string> fields = split_csv_line(line);
        if (fields.size() != 2) {
            return err("ALTER TABLE ADD COLUMN FROM CSV: row " + std::to_string(li + 1) +
                       " must have exactly 2 fields");
        }
        std::string pk_raw = trim_csv_field(fields[static_cast<size_t>(pk_csv_idx)]);
        std::string new_raw = fields[static_cast<size_t>(new_csv_idx)];
        if (pk_to_new_raw.count(pk_raw))
            return err("ALTER TABLE ADD COLUMN FROM CSV: duplicate primary key in file: " + pk_raw);
        pk_to_new_raw[pk_raw] = new_raw;
    }

    if (pk_to_new_raw.empty())
        return err("ALTER TABLE ADD COLUMN FROM CSV: no data rows");

    auto existing_rows = storage_.readAllRows(current_db_, q->table_name);
    std::set<std::string> pk_in_table;
    for (const auto& row : existing_rows) {
        if (pk_idx >= static_cast<int>(row.size()))
            return err("ALTER TABLE ADD COLUMN FROM CSV: internal row/pk mismatch");
        pk_in_table.insert(cell_to_where_string(row[static_cast<size_t>(pk_idx)]));
    }

    if (pk_to_new_raw.size() != pk_in_table.size())
        return err("ALTER TABLE ADD COLUMN FROM CSV: CSV row count (" +
                   std::to_string(pk_to_new_raw.size()) + ") must equal table row count (" +
                   std::to_string(pk_in_table.size()) + ")");

    for (const auto& kv : pk_to_new_raw) {
        if (!pk_in_table.count(kv.first))
            return err("ALTER TABLE ADD COLUMN FROM CSV: primary key not in table: " + kv.first);
    }

    if (!storage_.alterTableAddColumn(current_db_, q->table_name, new_cd))
        return err("ALTER TABLE ADD COLUMN failed (duplicate column or table missing).");

    auto s_after = storage_.getTableSchema(current_db_, q->table_name);
    const int new_col_idx = static_cast<int>(s_after.columns.size()) - 1;
    if (s_after.columns[static_cast<size_t>(new_col_idx)].name != new_cd.name)
        return err("ALTER TABLE ADD COLUMN FROM CSV: internal schema mismatch");

    std::vector<Row> rows_after = storage_.readAllRows(current_db_, q->table_name);
    std::vector<std::pair<Row, Row>> updates;
    updates.reserve(rows_after.size());

    for (const auto& row : rows_after) {
        std::string pk_lex = cell_to_where_string(row[static_cast<size_t>(pk_idx)]);
        auto it = pk_to_new_raw.find(pk_lex);
        if (it == pk_to_new_raw.end())
            return err("ALTER TABLE ADD COLUMN FROM CSV: missing CSV row for pk: " + pk_lex);

        CellValue new_cell = cell_from_csv_field(s_after.columns[static_cast<size_t>(new_col_idx)], it->second);
        if (new_cd.not_null && !new_cell.has_value())
            return err("NOT NULL violation for new column '" + new_cd.name + "' at pk " + pk_lex);

        Row new_row = row;
        new_row[static_cast<size_t>(new_col_idx)] = std::move(new_cell);
        updates.push_back({row, std::move(new_row)});
    }

    if (new_cd.unique) {
        std::set<std::string> seen;
        for (const auto& pr : updates) {
            const auto& v = pr.second[static_cast<size_t>(new_col_idx)];
            if (!v.has_value())
                continue;
            std::string lex = cell_to_where_string(v);
            if (!seen.insert(lex).second)
                return err("UNIQUE constraint violation on new column value: " + lex);
        }
    }

    const ColumnDef& new_col_def = s_after.columns[static_cast<size_t>(new_col_idx)];
    if (!new_col_def.fk_ref_table.empty()) {
        TableSchema ps = storage_.getTableSchema(current_db_, new_col_def.fk_ref_table);
        int pi = colIndex(ps, "", new_col_def.fk_ref_column);
        if (pi < 0) throw std::runtime_error("Invalid FK reference column");
        for (const auto& pr : updates) {
            const Row& new_row = pr.second;
            if (!new_row[static_cast<size_t>(new_col_idx)].has_value())
                continue;
            const std::string val =
                cell_to_where_string(new_row[static_cast<size_t>(new_col_idx)]);
            bool found = false;
            if (pi == ps.primary_key_index) {
                if (!storage_.findRow(current_db_, new_col_def.fk_ref_table, val).empty())
                    found = true;
            } else if (storage_.hasIndex(current_db_, new_col_def.fk_ref_table,
                                         new_col_def.fk_ref_column)) {
                if (!storage_.indexLookup(current_db_, new_col_def.fk_ref_table,
                                          new_col_def.fk_ref_column, val)
                         .empty())
                    found = true;
            } else {
                auto prs = storage_.readAllRows(current_db_, new_col_def.fk_ref_table);
                for (const auto& pr2 : prs) {
                    if (cell_to_where_string(pr2[pi]) == val) {
                        found = true;
                        break;
                    }
                }
            }
            if (!found)
                return err("FOREIGN KEY violation: value '" + val + "' not found in " +
                           new_col_def.fk_ref_table);
        }
    }

    for (const auto& pr : updates) {
        performUpdate(current_db_, q->table_name, s_after, pr.first, pr.second);
        storage_.upsertClusterRowWal(current_db_, q->table_name, s_after, &pr.first, pr.second);
    }

    return ok("Added column and updated " + std::to_string(updates.size()) + " row(s) from CSV.");
}
json Executor::execCreateIndex(const CreateIndexStatement* q) { 
    requireDB();
    checkPermission(q->table_name, "ALTER");

    if (storage_.createIndex(current_db_, q->table_name, q->index_name, q->column_name)) return ok("Created.");
     return err("Failed."); 
}
json Executor::execDropIndex(const DropIndexStatement* q) { 
    requireDB();
    checkPermission(q->table_name, "ALTER");

    if (storage_.dropIndex(current_db_, q->table_name, q->index_name)) return ok("Dropped."); 
    return err("Failed."); 
}

json Executor::execShow(const ShowStatement* q) {
    switch (q->type) {
        case ShowStatement::DATABASES: return execShowDatabases();
        case ShowStatement::TABLES: return execShowTables();
        case ShowStatement::COLUMNS: return execShowColumns(q->table_name);
        case ShowStatement::INDEX: return execShowIndex(q->table_name);
        case ShowStatement::CREATE_TABLE: return execShowCreateTable(q->table_name);
        default: return err("Unknown SHOW type");
    }
}

json Executor::execShowDatabases() {
    auto dbs = storage_.listDatabases();
    json rows = json::array();
    for (const auto& db : dbs) {
        if (db == "system" && !isAdmin()) continue;
        rows.push_back({db});
    }
    return {{"success", true}, {"columns", {"Database"}}, {"rows", rows}};
}

json Executor::execShowTables() {
    requireDB();
    auto tables = storage_.listTables(current_db_);
    json rows = json::array();
    for (const auto& t : tables) rows.push_back({t});
    return {{"success", true}, {"columns", {"Tables_in_" + current_db_}}, {"rows", rows}};
}

json Executor::execShowColumns(const std::string& table_name) {
    requireDB();
    auto s = storage_.getTableSchema(current_db_, table_name);
    json rows = json::array();
    for (size_t i = 0; i < s.columns.size(); ++i) {
        const auto& c = s.columns[i];
        std::string extra;
        if (c.is_autoincrement) extra = "auto_increment";

        std::string key;
        if ((int)i == s.primary_key_index) key = "PRI";
        else if (c.unique) key = "UNI";

        rows.push_back({
            c.name,
            c.type,
            c.not_null ? "NO" : "YES",
            key,
            c.has_default ? c.default_value : "NULL",
            extra
        });
    }
    return {{"success", true}, {"columns", {"Field", "Type", "Null", "Key", "Default", "Extra"}}, {"rows", rows}};
}

json Executor::execShowIndex(const std::string& table_name) {
    requireDB();
    auto s = storage_.getTableSchema(current_db_, table_name);
    json rows = json::array();
    // Primary key is an implicit index
    rows.push_back({table_name, "0", "PRIMARY", "1", s.columns[s.primary_key_index].name, "A", "NULL", "", ""});

    for (const auto& idx : s.indexes) {
        rows.push_back({table_name, "1", idx.index_name, "1", idx.column_name, "A", "NULL", "", ""});
    }
    return {{"success", true}, {"columns", {"Table", "Non_unique", "Key_name", "Seq_in_index", "Column_name", "Collation", "Cardinality", "Sub_part", "Packed"}}, {"rows", rows}};
}

json Executor::execShowCreateTable(const std::string& table_name) {
    requireDB();
    auto s = storage_.getTableSchema(current_db_, table_name);
    std::ostringstream sql;
    sql << "CREATE TABLE " << table_name << " (\n";
    for (size_t i = 0; i < s.columns.size(); ++i) {
        const auto& c = s.columns[i];
        sql << "  " << c.name << " " << c.type;
        if (c.not_null) sql << " NOT NULL";
        if (c.unique && (int)i != s.primary_key_index) sql << " UNIQUE";
        if (c.has_default) sql << " DEFAULT " << c.default_value;
        if (c.is_autoincrement) sql << " AUTOINCREMENT";
        if ((int)i == s.primary_key_index) sql << " PRIMARY KEY";
        if (!c.fk_ref_table.empty()) {
            sql << " REFERENCES " << c.fk_ref_table << "(" << c.fk_ref_column << ")";
            if (c.on_delete != OnDeleteAction::NO_ACTION) {
                sql << " ON DELETE " << (c.on_delete == OnDeleteAction::CASCADE ? "CASCADE" : "SET NULL");
            }
            if (c.on_update != OnUpdateAction::NO_ACTION) {
                sql << " ON UPDATE " << (c.on_update == OnUpdateAction::CASCADE ? "CASCADE" : "SET NULL");
            }
        }
        if (i < s.columns.size() - 1) sql << ",";
        sql << "\n";
    }
    sql << ");";

    json rows = json::array();
    rows.push_back({table_name, sql.str()});
    return {{"success", true}, {"columns", {"Table", "Create Table"}}, {"rows", rows}};
}

void Executor::applyDefaultsAndAutoincrement(const std::string& db_name, const TableSchema& s, const std::string& table_name,
                                            Row& r, std::map<int, long>& last_ids) {
    for (size_t i = 0; i < s.columns.size(); ++i) {
        if (!r[i].has_value()) {
            if (s.columns[i].is_autoincrement) {
                if (last_ids.find(static_cast<int>(i)) == last_ids.end()) {
                    long max_id = 0;
                    auto all_rows = storage_.readAllRows(db_name, table_name);
                    for (const auto& ar : all_rows) {
                        try {
                            if (i < ar.size() && ar[i].has_value()) {
                                long id = std::stol(cell_to_where_string(ar[i]));
                                if (id > max_id) max_id = id;
                            }
                        } catch (...) {
                        }
                    }
                    last_ids[static_cast<int>(i)] = max_id;
                }
                r[i] = CellPrimitive{static_cast<int64_t>(++last_ids[static_cast<int>(i)])};
            } else if (s.columns[i].has_default) {
                std::string def_val = s.columns[i].default_value;
                if (def_val == "CURRENT_DATE") {
                    auto t = std::time(nullptr);
                    auto tm = *std::localtime(&t);
                    std::ostringstream oss;
                    oss << std::put_time(&tm, "%Y-%m-%d");
                    def_val = oss.str();
                }
                r[i] = coerce_string_to_cell_column(s.columns[i], def_val, false);
            }
        }
    }
}

json Executor::insertValidatedRows(const std::string& table_name, const TableSchema& s,
                                   std::vector<Row> evaluated_rows) {
    std::vector<Row> current_table_data;
    bool table_data_loaded = false;

    for (size_t row_idx = 0; row_idx < evaluated_rows.size(); ++row_idx) {
        const auto& r = evaluated_rows[row_idx];

        for (size_t i = 0; i < s.columns.size(); ++i) {
            if (s.columns[i].unique || static_cast<int>(i) == s.primary_key_index) {
                if (!r[i].has_value())
                    continue;

                const std::string val = cell_to_where_string(r[i]);

                for (size_t prev_idx = 0; prev_idx < row_idx; ++prev_idx) {
                    if (cell_to_where_string(evaluated_rows[prev_idx][i]) == val) {
                        return err("UNIQUE constraint violation: duplicate value '" + val +
                                   "' in insert list");
                    }
                }

                if (static_cast<int>(i) == s.primary_key_index) {
                    if (!storage_.findRow(current_db_, table_name, val).empty()) {
                        return err("PRIMARY KEY violation: duplicate key '" + val + "'");
                    }
                } else if (storage_.hasIndex(current_db_, table_name, s.columns[i].name)) {
                    if (!storage_.indexLookup(current_db_, table_name, s.columns[i].name, val).empty()) {
                        return err("UNIQUE constraint violation: duplicate value '" + val +
                                   "' in column '" + s.columns[i].name + "'");
                    }
                } else {
                    if (!table_data_loaded) {
                        current_table_data = storage_.readAllRows(current_db_, table_name);
                        table_data_loaded = true;
                    }
                    for (const auto& tr : current_table_data) {
                        if (cell_to_where_string(tr[i]) == val) {
                            return err("UNIQUE constraint violation: duplicate value '" + val +
                                       "' in column '" + s.columns[i].name + "'");
                        }
                    }
                }
            }
        }
    }

    std::unordered_map<std::string, TableSchema> fk_parent_schemas;
    for (const auto& r : evaluated_rows) {
        for (size_t i = 0; i < s.columns.size(); ++i) {
            if (!s.columns[i].fk_ref_table.empty()) {
                if (!r[i].has_value())
                    continue;
                const std::string val = cell_to_where_string(r[i]);
                const std::string& ref_t = s.columns[i].fk_ref_table;
                auto em = fk_parent_schemas.try_emplace(ref_t,
                    storage_.getTableSchema(current_db_, ref_t));
                const TableSchema& ps = em.first->second;
                int pi = colIndex(ps, "", s.columns[i].fk_ref_column);
                if (pi < 0) throw std::runtime_error("Invalid FK reference column");
                bool found = false;
                if (pi == ps.primary_key_index) {
                    if (!storage_.findRow(current_db_, s.columns[i].fk_ref_table, val).empty())
                        found = true;
                } else if (storage_.hasIndex(current_db_, s.columns[i].fk_ref_table,
                                               s.columns[i].fk_ref_column)) {
                    if (!storage_.indexLookup(current_db_, s.columns[i].fk_ref_table,
                                              s.columns[i].fk_ref_column, val)
                             .empty())
                        found = true;
                } else {
                    auto prs = storage_.readAllRows(current_db_, s.columns[i].fk_ref_table);
                    for (const auto& pr : prs) {
                        if (cell_to_where_string(pr[pi]) == val) {
                            found = true;
                            break;
                        }
                    }
                }
                if (!found)
                    return err("FOREIGN KEY violation: value '" + val + "' not found in " +
                               s.columns[i].fk_ref_table);
            }
        }
    }

    int c = 0;
    for (auto& r : evaluated_rows) {
        if (storage_.appendRows(current_db_, table_name, {r}) > 0) {
            storage_.indexInsertRow(current_db_, table_name, s, r);
            ++c;
        }
    }
    return ok(std::to_string(c) + " inserted.");
}

json Executor::execInsert(const InsertStatement* q) {
    requireDB();
    checkPermission(q->table_name, "INSERT");

    auto s = storage_.getTableSchema(current_db_, q->table_name);
    std::vector<Row> evaluated_rows;

    std::map<int, long> last_ids;

    for (auto& ivs : q->insert_values) {
        Row r(s.columns.size());
        if (q->insert_columns.empty()) {
            for (size_t i = 0; i < ivs.size() && i < r.size(); ++i) {
                r[i] = value_to_cell(s.columns[i], evaluateExpression(ivs[i].get(), Row(), s, nullptr, nullptr));
            }
        } else {
            for (size_t i = 0; i < ivs.size() && i < q->insert_columns.size(); ++i) {
                int idx = colIndex(s, "", q->insert_columns[i]);
                if (idx < 0) throw std::runtime_error("Unknown column: " + q->insert_columns[i]);
                r[static_cast<size_t>(idx)] =
                    value_to_cell(s.columns[static_cast<size_t>(idx)],
                                  evaluateExpression(ivs[i].get(), Row(), s, nullptr, nullptr));
            }
        }

        applyDefaultsAndAutoincrement(current_db_, s, q->table_name, r, last_ids);

        for (size_t i = 0; i < s.columns.size(); ++i) {
            if (s.columns[i].not_null && !r[i].has_value()) {
                return err("NOT NULL constraint violation: column '" + s.columns[i].name + "'");
            }
        }
        evaluated_rows.push_back(std::move(r));
    }

    return insertValidatedRows(q->table_name, s, std::move(evaluated_rows));
}

json Executor::execLoadCsv(const LoadCsvStatement* q) {
    requireDB();
    checkPermission(q->table_name, "INSERT");
    if (!storage_.tableExists(current_db_, q->table_name))
        return err("Table does not exist: " + q->table_name);

    auto s = storage_.getTableSchema(current_db_, q->table_name);

    if (!q->append) {
        auto existing = storage_.readAllRows(current_db_, q->table_name);
        if (!existing.empty()) {
            return err(
                "LOAD CSV: table '" + q->table_name +
                "' is not empty. Existing rows are never deleted by LOAD CSV; use APPEND to add "
                "rows from the file.");
        }
    }

    std::filesystem::path fpath = resolve_csv_path(storage_, current_db_, q->file_path);

    std::string raw;
    try {
        raw = read_file_all(fpath);
    } catch (const std::exception& e) {
        return err(e.what());
    }

    if (raw.empty())
        return err("CSV file is empty");

    std::vector<std::string> lines = split_lines_unix(raw);
    if (lines.empty())
        return err("CSV has no lines");

    std::string header_line = strip_cr(strip_utf8_bom(lines[0]));
    std::vector<std::string> header_fields = split_csv_line(header_line);
    if (header_fields.empty())
        return err("CSV header is empty");

    std::vector<int> table_col_per_csv;
    std::set<int> used_table_cols;
    for (const auto& hf : header_fields) {
        std::string col_name = trim_csv_field(hf);
        int ti = colIndex(s, "", col_name);
        if (ti < 0)
            return err("Unknown column in CSV header: " + col_name);
        if (!q->columns.empty()) {
            bool allowed = false;
            for (const auto& listed : q->columns) {
                if (s.columns[static_cast<size_t>(ti)].name == listed) {
                    allowed = true;
                    break;
                }
            }
            if (!allowed)
                return err("CSV column not allowed by LOAD list: " + col_name);
        }
        if (!used_table_cols.insert(ti).second)
            return err("Duplicate column in CSV header: " + col_name);
        table_col_per_csv.push_back(ti);
    }

    if (q->columns.empty()) {
        if (used_table_cols.size() != s.columns.size())
            return err("CSV must include every table column exactly once");
    } else {
        if (used_table_cols.size() != q->columns.size())
            return err("CSV header columns must match LOAD column list");
        for (const auto& listed : q->columns) {
            int lix = colIndex(s, "", listed);
            if (lix < 0)
                return err("Unknown column in LOAD list: " + listed);
            if (!used_table_cols.count(lix))
                return err("LOAD column missing from CSV header: " + listed);
        }
    }

    std::vector<Row> evaluated_rows;
    std::map<int, long> last_ids;

    for (size_t li = 1; li < lines.size(); ++li) {
        std::string line = strip_cr(lines[li]);
        if (trim_csv_field(line).empty())
            continue;

        std::vector<std::string> fields = split_csv_line(line);
        if (fields.size() != header_fields.size()) {
            return err("CSV row " + std::to_string(li + 1) + ": expected " +
                       std::to_string(header_fields.size()) + " fields, got " +
                       std::to_string(fields.size()));
        }

        Row r(s.columns.size());
        for (size_t j = 0; j < fields.size(); ++j) {
            int ti = table_col_per_csv[j];
            r[static_cast<size_t>(ti)] =
                cell_from_csv_field(s.columns[static_cast<size_t>(ti)], fields[j]);
        }

        applyDefaultsAndAutoincrement(current_db_, s, q->table_name, r, last_ids);

        for (size_t i = 0; i < s.columns.size(); ++i) {
            if (s.columns[i].not_null && !r[i].has_value()) {
                return err("NOT NULL constraint violation at CSV row " + std::to_string(li + 1) +
                           ": column '" + s.columns[i].name + "'");
            }
        }
        evaluated_rows.push_back(std::move(r));
    }

    if (evaluated_rows.empty())
        return ok("0 rows loaded (no data rows).");

    return insertValidatedRows(q->table_name, s, std::move(evaluated_rows));
}
json Executor::execUpdate(const UpdateStatement* q) {
    requireDB();
    checkPermission(q->table_name, "UPDATE");

    auto s = storage_.getTableSchema(current_db_, q->table_name);
    auto rows = storage_.readAllRows(current_db_, q->table_name);
    int u = 0;
    for (auto& row : rows) {
        if (q->where && !evalCondition(q->where.get(), row, s, nullptr, nullptr, outer_schema_, outer_row_)) continue;
        Row old = row; Row new_row = row; bool mod = false;
        for (auto& sc : q->set_clauses) {
            int idx = colIndex(s, "", sc.column);
            if (idx < 0) throw std::runtime_error("Unknown column: " + sc.column);
            new_row[static_cast<size_t>(idx)] =
                value_to_cell(s.columns[static_cast<size_t>(idx)],
                              evaluateExpression(sc.value.get(), row, s, nullptr, nullptr, outer_schema_,
                                                 outer_row_));
            mod = true;
        }
        if (mod) {
            performUpdate(current_db_, q->table_name, s, old, new_row);
            storage_.upsertClusterRowWal(current_db_, q->table_name, s, &old, new_row);
            row = new_row;
            u++;
        }
    }
    return {{"success", true}, {"rows_affected", u}, {"message", std::to_string(u) + " updated."}};
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

                if (cell_to_where_string(old_row[parent_idx]) !=
                    cell_to_where_string(new_row[parent_idx])) {
                    auto child_rows = storage_.readAllRows(db_name, child_table_name);
                    for (auto& cr : child_rows) {
                        if (cell_to_where_string(cr[child_idx]) ==
                            cell_to_where_string(old_row[parent_idx])) {
                            Row old_cr = cr;
                            if (child_col.on_update == OnUpdateAction::CASCADE) {
                                cr[child_idx] = new_row[parent_idx];
                            } else if (child_col.on_update == OnUpdateAction::SET_NULL) {
                                cr[child_idx] = std::nullopt;
                            } else {
                                throw std::runtime_error("FOREIGN KEY violation: ON UPDATE RESTRICT/NO_ACTION");
                            }
                            storage_.upsertClusterRowWal(db_name, child_table_name, child_s,
                                                        &old_cr, cr);
                            performUpdate(db_name, child_table_name, child_s, old_cr, cr);
                        }
                    }
                }
            }
        }
    }
}
json Executor::execDelete(const DeleteStatement* q) {
    requireDB();
    checkPermission(q->table_name, "DELETE");

    auto s = storage_.getTableSchema(current_db_, q->table_name); 
    auto rows = storage_.readAllRows(current_db_, q->table_name); 
    std::vector<Row> to_delete;
    for (auto& row : rows) {
        if (!q->where || evalCondition(q->where.get(), row, s, nullptr, nullptr, outer_schema_, outer_row_)) {
            to_delete.push_back(row);
        }
    }

    int total_deleted = 0;
    performDelete(current_db_, q->table_name, to_delete, total_deleted);
    return {{"success", true}, {"rows_affected", total_deleted}, {"message", std::to_string(total_deleted) + " deleted."}};
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
                    const std::string pval = cell_to_where_string(prow[parent_idx]);

                    auto child_rows = storage_.readAllRows(db_name, child_table_name);
                    std::vector<Row> referencing_rows;
                    for (const auto& cr : child_rows)
                        if (cell_to_where_string(cr[child_idx]) == pval)
                            referencing_rows.push_back(cr);

                    if (referencing_rows.empty()) continue;

                    if (child_col.on_delete == OnDeleteAction::CASCADE) {
                        performDelete(db_name, child_table_name, referencing_rows, total_deleted);
                    } else if (child_col.on_delete == OnDeleteAction::SET_NULL) {
                        for (auto& cr : child_rows) {
                            if (cell_to_where_string(cr[child_idx]) == pval) {
                                Row old_cr = cr;
                                cr[child_idx] = std::nullopt;
                                storage_.upsertClusterRowWal(db_name, child_table_name, child_s,
                                                            &old_cr, cr);
                            }
                        }
                    } else {
                        // NO_ACTION / RESTRICT
                        throw std::runtime_error("FOREIGN KEY violation: cannot delete row from '" + table_name + "' (referenced by '" + child_table_name + "')");
                    }
                }
            }
        }
    }

    // 2. Physical delete from current table (cluster + indexes via WAL row records)
    for (const auto& dr : rows_to_delete) {
        storage_.deleteClusterRowWal(db_name, table_name, s, dr);
        total_deleted++;
    }
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
            int cix = colIndex(s, "", col->column);
            Value v = {cell_to_where_string((cix >= 0 && cix < (int)row.size()) ? row[cix]
                                                                         : CellValue{}),
                       cix >= 0 ? s.columns[cix].type : "TEXT"};
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
            size_t j = 0;
            for (auto& val : r_json) {
                if (j < m.columns.size())
                    r.push_back(coerce_string_to_cell_column(m.columns[j], val.get<std::string>(),
                                                             false));
                ++j;
            }
            rows.push_back(std::move(r));
        }
    } else {
        requireDB();
        checkPermission(q->table_name, "SELECT");
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
        checkPermission(jc.table_name, "SELECT");
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
                std::string key = cell_to_where_string(lr[li]);
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
                    Row c = lr;
                    Row pad(rs.columns.size());
                    c.insert(c.end(), pad.begin(), pad.end());
                    res.push_back(std::move(c));
                }
            }
        } else if (USE_HASH_JOIN) {
            // Hash Join implementation for all remaining JOIN types (INNER, LEFT, RIGHT, FULL)
            auto rr = storage_.readAllRows(current_db_, jc.table_name);
            std::unordered_map<std::string, std::vector<size_t>> hash_map;
            for (size_t i = 0; i < rows.size(); ++i) {
                hash_map[cell_to_where_string(rows[i][li])].push_back(i);
            }
            std::vector<bool> left_matched(rows.size(), false);
            std::vector<bool> right_matched(rr.size(), false);

            for (size_t i = 0; i < rr.size(); ++i) {
                std::string r_key = cell_to_where_string(rr[i][ri]);
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
                        Row c = rows[i];
                        Row pad(rs.columns.size());
                        c.insert(c.end(), pad.begin(), pad.end());
                        res.push_back(std::move(c));
                    }
                }
            }
            if (jc.join_type == JoinClause::RIGHT || jc.join_type == JoinClause::FULL) {
                for (size_t i = 0; i < rr.size(); ++i) {
                    if (!right_matched[i]) {
                        Row c(m.columns.size());
                        c.insert(c.end(), rr[i].begin(), rr[i].end());
                        res.push_back(std::move(c));
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
                    if (cell_to_where_string(lr[li]) == cell_to_where_string(rr[i][ri])) {
                        Row c = lr; c.insert(c.end(), rr[i].begin(), rr[i].end());
                        res.push_back(std::move(c)); matched = true; rm[i] = true;
                    }
                }
                if (!matched && (jc.join_type == JoinClause::LEFT || jc.join_type == JoinClause::FULL)) {
                    Row c = lr;
                    Row pad(rs.columns.size());
                    c.insert(c.end(), pad.begin(), pad.end());
                    res.push_back(std::move(c));
                }
            }
            if (jc.join_type == JoinClause::RIGHT || jc.join_type == JoinClause::FULL) {
                for (size_t i = 0; i < rr.size(); ++i) {
                    if (!rm[i]) {
                        Row c(m.columns.size());
                        c.insert(c.end(), rr[i].begin(), rr[i].end());
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
    if (q->where) { std::vector<Row> f; for (auto& r : rows) if (evalCondition(q->where.get(), r, m, nullptr, nullptr, outer_schema_, outer_row_)) f.push_back(r); rows = std::move(f); }

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
            std::string k;
            for (auto& gb : q->group_by) {
                int i = colIndex(m, "", gb);
                k += (i >= 0 ? cell_to_where_string(row[i]) : "") + "|";
            }
            auto& g = g_map[k]; if (g.rep.empty()) g.rep = row;
            for (auto& aggr_pair : unique_aggr_exprs) {
                auto& aggr = aggr_pair.first;
                bool is_distinct = aggr_pair.second;
                auto& st = g.st[aggr];

                std::string val_to_add;
                if (aggr.second == "*") val_to_add = "*";
                else {
                    int i = colIndex(m, "", aggr.second);
                    if (i >= 0) val_to_add = cell_to_where_string(row[i]);
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
            g.rep = Row(m.columns.size());
            for (auto& aggr_pair : unique_aggr_exprs) {
                auto& st = g.st[aggr_pair.first];
                st.count = 0;
                st.sum = 0;
                st.initialized = false;
            }
        }
        for (auto& kv : g_map) if (!q->having || evalCondition(q->having.get(), kv.second.rep, m, &kv.second.st, &q->select_columns, outer_schema_, outer_row_)) groups.push_back(std::move(kv.second));
    } else {
        for (auto& r : rows) { Group g; g.rep = r; groups.push_back(std::move(g)); }
    }

    if (!q->order_by.empty() && !sorted_by_index) {
        std::sort(groups.begin(), groups.end(), [&](const Group& a, const Group& b) {
            for (auto& ob : q->order_by) {
                Value va = evaluateExpression(ob.expr.get(), a.rep, m, &a.st, &q->select_columns, outer_schema_, outer_row_);
                Value vb = evaluateExpression(ob.expr.get(), b.rep, m, &b.st, &q->select_columns, outer_schema_, outer_row_);
                int cmp = compareValues(va, vb);
                if (cmp != 0) return ob.asc ? cmp < 0 : cmp > 0;
            }
            return false;
        });
    }

    std::vector<std::vector<std::string>> res_r;
    std::vector<std::string> cn;
    if (q->select_all) for (auto& c : m.columns) cn.push_back(c.name);
    else for (auto& sc : q->select_columns) {
        if (!sc.alias.empty()) cn.push_back(sc.alias);
        else if (auto ce = dynamic_cast<ColumnExpression*>(sc.expr.get())) cn.push_back(ce->column);
        else if (dynamic_cast<AggregateExpression*>(sc.expr.get())) cn.push_back("aggr");
        else cn.push_back("expr");
    }

    for (auto& g : groups) {
        std::vector<std::string> out;
        if (q->select_all) {
            out.reserve(g.rep.size());
            for (auto& cv : g.rep) out.push_back(cell_to_where_string(cv));
        } else {
            for (auto& sc : q->select_columns) {
                out.push_back(
                    evaluateExpression(sc.expr.get(), g.rep, m, &g.st, &q->select_columns, outer_schema_, outer_row_).val);
            }
        }
        res_r.push_back(std::move(out));
    }

    if (q->distinct) {
        std::set<std::vector<std::string>> unique_rows;
        std::vector<std::vector<std::string>> distinct_rows;
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
    for (int i = s_i; i < e_i; ++i) {
        json jr = json::array();
        for (auto& v : res_r[i]) jr.push_back(v);
        res["rows"].push_back(jr);
    }
    return res;
}

std::vector<Row> Executor::execute_subquery(const SelectStatement* q, const TableSchema* os, const Row* orow) {
    const TableSchema* ps = outer_schema_; const Row* pr = outer_row_; outer_schema_ = os; outer_row_ = orow;
    auto r = execSelect(q); outer_schema_ = ps; outer_row_ = pr; std::vector<Row> rws;
    if (r["success"]) {
        TableSchema inferred;
        inferred.table_name = "";
        inferred.columns.reserve(r["columns"].size());
        for (auto& cname : r["columns"]) {
            ColumnDef d;
            d.name = cname.get<std::string>();
            d.type = "TEXT";
            inferred.columns.push_back(std::move(d));
        }
        for (auto& rr : r["rows"]) {
            Row row;
            size_t j = 0;
            for (auto& v : rr) {
                if (j < inferred.columns.size())
                    row.push_back(
                        coerce_string_to_cell_column(inferred.columns[j], v.get<std::string>(), false));
                ++j;
            }
            rws.push_back(std::move(row));
        }
    }
    return rws;
}

json Executor::execCreateUser(const CreateUserStatement* q) {
    // Только глобальный админ может создавать пользователей через CREATE USER
    if (!isAdmin()) return err("Only global admin can use CREATE USER. Use REGISTER for self-registration.");

    auto existing = storage_.indexLookup("system", "sys_users", "username", q->username);
    if (!existing.empty()) return err("User already exists.");

    auto sch = storage_.getTableSchema("system", "sys_users");
    Row row(sch.columns.size());
    row[1] = coerce_string_to_cell_column(sch.columns[1], q->username, false);
    row[2] = coerce_string_to_cell_column(sch.columns[2], hashPassword(q->password), false);
    row[3] = coerce_string_to_cell_column(sch.columns[3], "0", false);
    std::map<int, long> dummy;
    applyDefaultsAndAutoincrement("system", sch, "sys_users", row, dummy);
    storage_.appendRows("system", "sys_users", {row});
    storage_.indexInsertRow("system", "sys_users", sch, row);

    return ok("User '" + q->username + "' created.");
}

json Executor::execCreateRole(const CreateRoleStatement* q) {
    return err("CREATE ROLE is deprecated. The new RBAC model uses database ownership. "
               "Use GRANT DDL ON <db> TO <user> to grant DDL privileges.");
}

nlohmann::json Executor::execSetUser(const SetUserStatement* q) {
    // SET USER теперь работает глобально через system DB
    auto users = storage_.indexLookup("system", "sys_users", "username", q->username);
    if (users.empty()) return err("User '" + q->username + "' does not exist.");

    Row user_row = storage_.findRow("system", "sys_users", users[0]);
    std::string stored_pass = "";
    if (user_row.size() > 2 && user_row[2].has_value()) {
        stored_pass = cell_to_where_string(user_row[2]);
    }
    
    if (stored_pass != hashPassword(q->password)) return err("Invalid password for user '" + q->username + "'.");

    current_user_ = q->username;
    json res = ok("Context switched to user: " + current_user_);
    res["current_user"] = current_user_;
    return res;
}

json Executor::execGrantRole(const GrantRoleStatement* q) {
    return err("GRANT ROLE is deprecated. The new RBAC model uses database ownership. "
               "Use GRANT DDL ON <db> TO <user> to grant DDL privileges.");
}

json Executor::execGrant(const GrantStatement* q) {
    return err("Legacy GRANT <privilege> ON <table> TO <role> is deprecated. "
               "Use GRANT DDL ON <db> TO <user> instead.");
}

json Executor::execRegister(const RegisterStatement* q) {
    // Remove hardcoded "admin" rejection, anyone can register but they won't be admin.
    auto existing = storage_.indexLookup("system", "sys_users", "username", q->username);
    if (!existing.empty()) return err("User '" + q->username + "' already exists.");
    if (q->password.empty()) return err("Password cannot be empty.");

    auto sch = storage_.getTableSchema("system", "sys_users");
    Row row(sch.columns.size());
    row[1] = coerce_string_to_cell_column(sch.columns[1], q->username, false);
    row[2] = coerce_string_to_cell_column(sch.columns[2], hashPassword(q->password), false);
    row[3] = coerce_string_to_cell_column(sch.columns[3], "0", false);
    std::map<int, long> dummy;
    applyDefaultsAndAutoincrement("system", sch, "sys_users", row, dummy);
    storage_.appendRows("system", "sys_users", {row});
    storage_.indexInsertRow("system", "sys_users", sch, row);

    current_user_ = q->username;
    json res = ok("User '" + q->username + "' registered and logged in.");
    res["current_user"] = current_user_;
    res["is_admin"] = isAdmin();
    return res;
}

json Executor::execLogin(const LoginStatement* q) {
    auto users = storage_.indexLookup("system", "sys_users", "username", q->username);
    if (users.empty()) return err("User '" + q->username + "' does not exist.");

    Row user_row = storage_.findRow("system", "sys_users", users[0]);
    std::string stored_pass = "";
    if (user_row.size() > 2 && user_row[2].has_value()) {
        stored_pass = cell_to_where_string(user_row[2]);
    }
    if (stored_pass != hashPassword(q->password)) return err("Invalid password.");

    current_user_ = q->username;
    json res = ok("Logged in as '" + current_user_ + "'.");
    res["current_user"] = current_user_;
    res["is_admin"] = isAdmin();
    return res;
}

json Executor::execChangePassword(const ChangePasswordStatement* q) {
    if (current_user_.empty()) return err("Not authenticated.");
    auto users = storage_.indexLookup("system", "sys_users", "username", current_user_);
    if (users.empty()) return err("User not found.");

    Row user_row = storage_.findRow("system", "sys_users", users[0]);
    std::string stored_pass;
    if (user_row.size() > 2 && user_row[2].has_value()) {
        stored_pass = cell_to_where_string(user_row[2]);
    }
    if (stored_pass != hashPassword(q->old_password)) return err("Invalid current password.");
    if (q->new_password.empty()) return err("New password cannot be empty.");

    auto sch = storage_.getTableSchema("system", "sys_users");
    Row new_row = user_row;
    new_row[2] = coerce_string_to_cell_column(sch.columns[2], hashPassword(q->new_password), false);

    performUpdate("system", "sys_users", sch, user_row, new_row);
    storage_.upsertClusterRowWal("system", "sys_users", sch, &user_row, new_row);

    return ok("Password updated.");
}

json Executor::execGrantDdl(const GrantDdlStatement* q) {
    if (current_user_.empty()) return err("Not authenticated.");
    
    // Проверяем, что БД существует
    if (!storage_.databaseExists(q->db_name)) return err("Database '" + q->db_name + "' does not exist.");

    // Только владелец или админ может выдавать права на БД
    std::string owner = storage_.getDbOwner(q->db_name);
    if (!isAdmin() && owner != current_user_) {
        return err("Permission denied: only owner '" + owner + "' or admin can grant privileges on '" + q->db_name + "'.");
    }

    // Проверяем, что юзер существует
    auto user_pks = storage_.indexLookup("system", "sys_users", "username", q->username);
    if (user_pks.empty()) return err("User '" + q->username + "' does not exist.");

    const std::string& role_to_grant = q->role;
    if (role_to_grant != "ddl" && role_to_grant != "editor") {
        return err("Unsupported grant role '" + role_to_grant + "'. Use GRANT DDL or GRANT EDITOR.");
    }

    if (storage_.hasDbGrantRole(q->db_name, q->username, role_to_grant)) {
        return ok("User '" + q->username + "' already has role '" + role_to_grant + "' on '" + q->db_name + "'.");
    }

    auto sch = storage_.getTableSchema("system", "sys_db_grants");
    Row row(sch.columns.size());
    row[1] = coerce_string_to_cell_column(sch.columns[1], q->db_name, false);
    row[2] = coerce_string_to_cell_column(sch.columns[2], q->username, false);
    row[3] = coerce_string_to_cell_column(sch.columns[3], role_to_grant, false);
    std::map<int, long> dummy;
    applyDefaultsAndAutoincrement("system", sch, "sys_db_grants", row, dummy);
    storage_.appendRows("system", "sys_db_grants", {row});
    storage_.indexInsertRow("system", "sys_db_grants", sch, row);

    return ok("Granted " + role_to_grant + " on '" + q->db_name + "' to user '" + q->username + "'.");
}

json Executor::execRevokeDdl(const RevokeDdlStatement* q) {
    if (current_user_.empty()) return err("Not authenticated.");
    
    if (!storage_.databaseExists(q->db_name)) return err("Database '" + q->db_name + "' does not exist.");

    std::string owner = storage_.getDbOwner(q->db_name);
    if (!isAdmin() && owner != current_user_) {
        return err("Permission denied: only owner '" + owner + "' or admin can revoke privileges on '" + q->db_name + "'.");
    }

    if (!storage_.hasDbGrantRole(q->db_name, q->username, q->role)) {
        return err("User '" + q->username + "' does not have role '" + q->role + "' on '" + q->db_name + "'.");
    }

    auto grants = storage_.readAllRows("system", "sys_db_grants");
    std::vector<Row> to_delete;
    for (const auto& row : grants) {
        if (row.size() >= 4 && row[1].has_value() && row[2].has_value() && row[3].has_value()) {
            if (cell_to_where_string(row[1]) == q->db_name &&
                cell_to_where_string(row[2]) == q->username &&
                cell_to_where_string(row[3]) == q->role) {
                to_delete.push_back(row);
                break;
            }
        }
    }

    int total_deleted = 0;
    if (!to_delete.empty()) {
        performDelete("system", "sys_db_grants", to_delete, total_deleted);
    }
    return ok("Revoked role '" + q->role + "' on '" + q->db_name + "' from user '" + q->username + "'.");
}

json Executor::execLogout() {
    current_user_ = "";
    current_db_ = "";
    json res = ok("Logged out successfully.");
    res["type"] = "logout";
    return res;
}

bool Executor::isAdmin() const {
    if (current_user_.empty()) return false;
    auto user_pks = storage_.indexLookup("system", "sys_users", "username", current_user_);
    if (user_pks.empty()) return false;
    Row row = storage_.findRow("system", "sys_users", user_pks[0]);
    if (row.size() > 3 && row[3].has_value() && cell_to_where_string(row[3]) == "1") {
        return true;
    }
    return false;
}

} // namespace db
