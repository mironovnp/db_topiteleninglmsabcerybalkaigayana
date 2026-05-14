#pragma once
#include "engine/storage/storage.hpp"
#include "engine/parser.hpp"
#include <nlohmann/json.hpp>
#include <string>
#include <map>
#include <vector>
#include <set>
#include <mutex>

namespace db {

struct Value {
    std::string val;
    std::string type; // INT, FLOAT, TEXT, BOOL, NULL
};

class Executor {
public:
    explicit Executor(const std::string& data_dir = "data");
    nlohmann::json execute(const std::string& sql);

    // Установка контекста БД для текущего потока HTTP-сервера
    void setThreadLocalContext(const std::string& db) {
        current_db_ = db;
    }

    void setThreadLocalUser(const std::string& user) {
        current_user_ = user;
    }

private:
    Storage storage_;

    // МНОГОПОТОЧНОСТЬ: Потоко-локальные переменные контекста.
    inline thread_local static std::string current_db_;
    inline thread_local static const TableSchema* outer_schema_ = nullptr;
    inline thread_local static const Row* outer_row_ = nullptr;

    inline thread_local static std::string current_user_ = ""; 

    // --- Session & RBAC ---
    std::unordered_map<std::string, bool> priv_cache_;
    std::mutex priv_cache_mutex_; // <--- ЗАЩИТА КЭША ОТ ГОНКИ ДАННЫХ --->

    nlohmann::json execCreateDB(const CreateDatabaseStatement* q);
    nlohmann::json execDropDB(const DropDatabaseStatement* q);
    nlohmann::json execCreateTable(const CreateTableStatement* q);
    nlohmann::json execDropTable(const DropTableStatement* q);
    nlohmann::json execSelect(const SelectStatement* q);
    nlohmann::json execInsert(const InsertStatement* q);
    nlohmann::json execUpdate(const UpdateStatement* q);
    nlohmann::json execDelete(const DeleteStatement* q);
    nlohmann::json execUse(const UseDatabaseStatement* q);
    nlohmann::json execAlterTable(const AlterTableStatement* q);
    nlohmann::json execCreateIndex(const CreateIndexStatement* q);
    nlohmann::json execDropIndex(const DropIndexStatement* q);
    nlohmann::json execShow(const ShowStatement* q);
    nlohmann::json execLoadCsv(const LoadCsvStatement* q);
    nlohmann::json execAlterTableAddColumnFromCsv(const AlterTableStatement* q);

    nlohmann::json insertValidatedRows(const std::string& table_name, const TableSchema& s,
                                       std::vector<Row> evaluated_rows);
    void applyDefaultsAndAutoincrement(const std::string& db_name, const TableSchema& s, const std::string& table_name, Row& r,
                                       std::map<int, long>& last_ids);

    void checkPermission(const std::string& table_name, const std::string& privilege);

    nlohmann::json execCreateUser(const CreateUserStatement* q);
    nlohmann::json execCreateRole(const CreateRoleStatement* q);
    nlohmann::json execSetUser(const SetUserStatement* q);
    nlohmann::json execGrantRole(const GrantRoleStatement* q);
    nlohmann::json execGrant(const GrantStatement* q);
    nlohmann::json execRegister(const RegisterStatement* q);
    nlohmann::json execLogin(const LoginStatement* q);
    nlohmann::json execChangePassword(const ChangePasswordStatement* q);
    nlohmann::json execGrantDdl(const GrantDdlStatement* q);
    nlohmann::json execRevokeDdl(const RevokeDdlStatement* q);
    nlohmann::json execLogout();
    nlohmann::json execBegin();
    nlohmann::json execCommit();
    nlohmann::json execRollback();

    nlohmann::json execShowDatabases();
    nlohmann::json execShowTables();
    nlohmann::json execShowColumns(const std::string& table_name);
    nlohmann::json execShowIndex(const std::string& table_name);
    nlohmann::json execShowCreateTable(const std::string& table_name);

    void requireDB() const;

    struct AggrState {
        int count = 0;
        double sum = 0.0;
        double min_val = 0.0;
        double max_val = 0.0;
        bool initialized = false;
        std::set<std::string> seen_values;
    };

    Value evaluateExpression(const Expression* expr,
                             const Row& row,
                             const TableSchema& schema,
                             const std::map<std::pair<AggrFunc, std::string>, AggrState>* aggrs = nullptr,
                             const std::vector<SelectColumn>* select_cols = nullptr,
                             const TableSchema* outer_schema = nullptr,
                             const Row* outer_row = nullptr);

    bool evalCondition(const Expression* expr, const Row& row, const TableSchema& schema,
                       const std::map<std::pair<AggrFunc, std::string>, AggrState>* aggrs = nullptr,
                       const std::vector<SelectColumn>* select_cols = nullptr,
                       const TableSchema* outer_schema = nullptr,
                       const Row* outer_row = nullptr);

    std::vector<Row> execute_subquery(const SelectStatement* q,
                                      const TableSchema* outer_schema = nullptr,
                                      const Row* outer_row = nullptr);

    void performDelete(const std::string& db_name, const std::string& table_name, const std::vector<Row>& rows_to_delete, int& total_deleted);
    void performUpdate(const std::string& db_name, const std::string& table_name, const TableSchema& s, const Row& old_row, const Row& new_row);

    int colIndex(const TableSchema& s, const std::string& table, const std::string& name) const;
    int compareValues(const Value& a, const Value& b) const;

    bool tryIndexScan(const SelectStatement* q, const TableSchema& schema, std::vector<Row>& out_rows);

    static nlohmann::json ok(const std::string& msg);
    static nlohmann::json err(const std::string& msg);
};

} // namespace db
