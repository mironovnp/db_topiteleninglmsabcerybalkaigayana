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

    struct SessionContext {
        std::string current_db;
        std::string current_user;
    };
    std::mutex sessions_mutex_;
    std::unordered_map<std::string, SessionContext> sessions_;
    std::atomic<unsigned long long> next_session_id_{1};

    void* svr_ptr_ = nullptr; // Using void* to avoid including httplib.h in header
};

} // namespace db
