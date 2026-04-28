#pragma once
#include "engine/executor.hpp"
#include <string>

namespace db {

class Server {
public:
    Server(const std::string& host, int port, const std::string& data_dir = "data");
    void start();

private:
    std::string host_;
    int port_;
    Executor executor_;
};

} // namespace db
