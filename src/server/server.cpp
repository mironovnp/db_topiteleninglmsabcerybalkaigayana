#include "server/server.hpp"
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <iostream>

namespace db {

using json = nlohmann::json;

Server::Server(const std::string& host, int port, const std::string& data_dir)
    : host_(host), port_(port), executor_(data_dir) {}

void Server::start() {
    httplib::Server svr;

    svr.Post("/query", [this](const httplib::Request& req, httplib::Response& res) {
        try {
            auto body = json::parse(req.body);
            std::string sql = body.value("sql", "");
            if (sql.empty()) {
                res.set_content(
                    json({{"success", false}, {"message", "Empty query"}}).dump(),
                    "application/json");
                return;
            }

            bool dry_run = body.value("dry_run", false);
            if (dry_run) {
                try {
                    db::Lexer lexer(sql);
                    db::Parser parser(lexer.tokenize());
                    parser.parse();
                } catch (const std::exception& e) {
                    std::string err = e.what();
                    if (err.find("<EOF>") != std::string::npos) {
                        res.set_content(json({{"success", true}, {"type", "incomplete"}}).dump(), "application/json");
                    } else {
                        res.set_content(json({{"success", false}, {"message", err}}).dump(), "application/json");
                    }
                    return;
                }
                res.set_content(json({{"success", true}, {"type", "incomplete"}}).dump(), "application/json");
                return;
            }

            auto result = executor_.execute(sql);
            res.set_content(result.dump(), "application/json");
        } catch (const std::exception& e) {
            res.set_content(
                json({{"success", false}, {"message", e.what()}}).dump(),
                "application/json");
        }
    });

    svr.Get("/ping", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("{\"status\":\"ok\"}", "application/json");
    });

    std::cout << "[databasetopit] Server listening on " << host_ << ":" << port_ << std::endl;
    svr.listen(host_, port_);
}

} // namespace db
