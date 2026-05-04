#pragma once
#include "engine/storage.hpp"
#include "engine/parser.hpp"
#include <nlohmann/json.hpp>
#include <string>
#include <map>

namespace db {

class Executor {
public:
    explicit Executor(const std::string& data_dir = "data");
    nlohmann::json execute(const std::string& sql);

private:
    Storage storage_;
    std::string current_db_;

    nlohmann::json execCreateDB(const ParsedQuery& q);
    nlohmann::json execDropDB(const ParsedQuery& q);
    nlohmann::json execCreateTable(const ParsedQuery& q);
    nlohmann::json execDropTable(const ParsedQuery& q);
    nlohmann::json execSelect(const ParsedQuery& q);
    nlohmann::json execInsert(const ParsedQuery& q);
    nlohmann::json execUpdate(const ParsedQuery& q);
    nlohmann::json execDelete(const ParsedQuery& q);
    nlohmann::json execUse(const ParsedQuery& q);
    nlohmann::json execAlterTable(const ParsedQuery& q);

    void requireDB() const;
    bool evalWhere(const WhereExpr& expr, const Row& row,
                   const TableSchema& schema) const;

    struct AggrState {
        int count = 0;
        double sum = 0.0;
        double min_val = 0.0;
        double max_val = 0.0;
        bool initialized = false;
    };
    
    // Evaluate HAVING condition using the representative row of a group and its aggregate states
    bool evalHaving(const WhereExpr& expr, const Row& row, const TableSchema& schema,
                    const std::map<std::pair<AggrFunc, std::string>, AggrState>& aggrs) const;

    int colIndex(const TableSchema& s, const std::string& name) const;
    int compareValues(const std::string& a, const std::string& b,
                      const std::string& type) const;

    static nlohmann::json ok(const std::string& msg);
    static nlohmann::json err(const std::string& msg);
};

} // namespace db
