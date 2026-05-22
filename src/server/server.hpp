#pragma once
#include "engine/executor.hpp"
#include <string>
#include <shared_mutex>
#include <atomic>
#include <mutex>
#include <unordered_map>

namespace db {

class Server {
public:
    Server(const std::string& host, int port, const std::string& data_dir = "data");
    void start();
    void stop();

private:
    std::string host_;
    int port_;
    Executor executor_;

    // Мьютекс для разделения блокировок на чтение и запись
    std::shared_mutex db_rw_mutex_;
    // Синхронизация транзакционного контекста сессии с общим Storage (не thread_local).
    std::mutex txn_bridge_mutex_;

    struct SessionContext {
        std::string current_db;
        std::string current_user;
        Storage::TransactionState txn{};
    };
    std::mutex sessions_mutex_;
    std::unordered_map<std::string, SessionContext> sessions_;
    std::atomic<unsigned long long> next_session_id_{1};

    nlohmann::json attach_session_context(const std::string& session_id, nlohmann::json result);

    struct QueryRunResult {
        nlohmann::json response;
        SessionContext session;
    };
    QueryRunResult runSessionQuery(const SessionContext& session, const std::string& sql, bool is_read_only);

    void* svr_ptr_ = nullptr; // Using void* to avoid including httplib.h in header
};

} // namespace db
