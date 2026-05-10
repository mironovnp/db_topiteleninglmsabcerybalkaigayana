#pragma once
#include "engine/executor.hpp"
#include <string>
#include <shared_mutex>

namespace db {

class Server {
public:
    Server(const std::string& host, int port, const std::string& data_dir = "data");
    void start();

private:
    std::string host_;
    int port_;
    Executor executor_;

    // Мьютекс для разделения блокировок на чтение и запись
    std::shared_mutex db_rw_mutex_;
};

} // namespace db
