#pragma once
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace db {

/// Result of a query execution
struct QueryResult {
    bool success;
    std::string message;
    std::string type;                              // "ddl", "select", "modify"
    int affected_rows = 0;
    std::vector<std::string> columns;
    std::vector<std::vector<std::string>> rows;
};

/// Facade pattern — hides all network logic behind a single interface
class DBClient {
public:
    DBClient();

    /// Connect to a running databasetopit server
    bool connect(const std::string& host, int port);

    /// Execute an SQL query and return the result
    QueryResult executeQuery(const std::string& sql, bool dry_run = false);

    /// Check if server is reachable
    bool ping();

private:
    std::string host_;
    int port_ = 0;
    bool connected_ = false;
};

} // namespace db
