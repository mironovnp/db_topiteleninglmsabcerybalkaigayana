#pragma once
#include "engine/storage.hpp"
#include "engine/parser.hpp"
#include <nlohmann/json.hpp>
#include <string>
#include <map>
#include <vector>

namespace db {

struct Value {
    std::string val;
    std::string type; // INT, FLOAT, TEXT, BOOL, NULL
};

class Executor {
public:
    explicit Executor(const std::string& data_dir = "data");
    nlohmann::json execute(const std::string& sql);

private:
    Storage storage_;
    std::string current_db_;

    const TableSchema* outer_schema_ = nullptr;
    const Row* outer_row_ = nullptr;

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

    void requireDB() const;

    struct AggrState {
        int count = 0;
        double sum = 0.0;
        double min_val = 0.0;
        double max_val = 0.0;
        bool initialized = false;
    };
    
    // Core expression evaluation method
    Value evaluateExpression(const Expression* expr, 
                             const Row& row, 
                             const TableSchema& schema,
                             const std::map<std::pair<AggrFunc, std::string>, AggrState>* aggrs = nullptr,
                             const TableSchema* outer_schema = nullptr,
                             const Row* outer_row = nullptr);

    bool evalCondition(const Expression* expr, const Row& row, const TableSchema& schema,
                       const std::map<std::pair<AggrFunc, std::string>, AggrState>* aggrs = nullptr,
                       const TableSchema* outer_schema = nullptr,
                       const Row* outer_row = nullptr);

    std::vector<Row> execute_subquery(const SelectStatement* q,
                                      const TableSchema* outer_schema = nullptr,
                                      const Row* outer_row = nullptr);

    void performDelete(const std::string& db_name, const std::string& table_name, const std::vector<Row>& rows_to_delete, int& total_deleted);

    int colIndex(const TableSchema& s, const std::string& table, const std::string& name) const;
    int compareValues(const Value& a, const Value& b) const;

    bool tryIndexScan(const SelectStatement* q, const TableSchema& schema, std::vector<Row>& out_rows);

    static nlohmann::json ok(const std::string& msg);
    static nlohmann::json err(const std::string& msg);
};

} // namespace db
