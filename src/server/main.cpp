#include "server/server.hpp"
#include <iostream>
#include <string>

int main(int argc, char* argv[]) {
    std::string host = "0.0.0.0";
    int port = 8080;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--host" && i + 1 < argc) host = argv[++i];
        else if (arg == "--port" && i + 1 < argc) port = std::stoi(argv[++i]);
    }

    db::Server server(host, port);
    server.start();
    return 0;
}
