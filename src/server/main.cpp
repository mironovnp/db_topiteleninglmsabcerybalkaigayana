#include "server/server.hpp"
#include <iostream>
#include <string>
#include <csignal>

namespace {
    db::Server* global_server_ptr = nullptr;
}

void signal_handler(int signal) {
    if (signal == SIGINT && global_server_ptr) {
        std::cout << "\n[*] Interrupt signal received. Shutting down server gracefully...\n";
        global_server_ptr->stop();
    }
}

int main(int argc, char* argv[]) {
    std::string host = "0.0.0.0";
    int port = 8080;
    std::string data_dir = "data";

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--host" && i + 1 < argc) host = argv[++i];
        else if (arg == "--port" && i + 1 < argc) port = std::stoi(argv[++i]);
        else if ((arg == "--data-dir" || arg == "--data") && i + 1 < argc) data_dir = argv[++i];
    }

    db::Server server(host, port, data_dir);
    global_server_ptr = &server;
    std::signal(SIGINT, signal_handler);

    server.start();
    return 0;
}
