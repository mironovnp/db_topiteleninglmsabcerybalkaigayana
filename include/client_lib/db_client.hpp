#pragma once
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace db {

struct QueryResult {
    bool success;
    std::string message;
    std::string type;                              // "ddl", "select", "modify"
    int affected_rows = 0;
    std::vector<std::string> columns;
    std::vector<std::vector<std::string>> rows;
    std::string current_db;
    std::string current_user;
};

class DBClient {
public:
    DBClient();
    bool connect(const std::string& host, int port);
    QueryResult executeQuery(const std::string& sql, bool dry_run = false);
    QueryResult executeText2Sql(const std::string& request);
    bool ping();

private:
    std::string host_;
    int port_ = 0;
    bool connected_ = false;

    // СОСТОЯНИЕ: Клиент хранит имя своей текущей БД
    std::string current_db_;
    std::string current_user_;
    std::string session_id_;
};

} // namespace db
