#include "client_lib/db_client.hpp"
#include <httplib.h>
#include <iostream>

namespace db {

DBClient::DBClient() = default;

bool DBClient::connect(const std::string& host, int port) {
    host_ = host;
    port_ = port;
    connected_ = ping();
    return connected_;
}

bool DBClient::ping() {
    try {
        httplib::Client cli(host_, port_);
        cli.set_connection_timeout(3);
        auto res = cli.Get("/ping");
        return res && res->status == 200;
    } catch (...) {
        return false;
    }
}

QueryResult DBClient::executeQuery(const std::string& sql, bool dry_run) {
    QueryResult qr;
    qr.success = false;

    if (!connected_) {
        qr.message = "Not connected to server.";
        return qr;
    }

    try {
        httplib::Client cli(host_, port_);
        cli.set_connection_timeout(10);
        cli.set_read_timeout(30);

        nlohmann::json body;
        body["sql"] = sql;
        body["dry_run"] = dry_run;
        body["current_db"] = current_db_; // Отправляем серверу текущую базу клиента
        body["current_user"] = current_user_;
        if (!session_id_.empty()) {
            body["session_id"] = session_id_;
        }

        auto res = cli.Post("/query", body.dump(), "application/json");
        if (!res) {
            qr.message = "Connection failed.";
            return qr;
        }

        auto j = nlohmann::json::parse(res->body);
        qr.success = j.value("success", false);
        qr.message = j.value("message", "");
        qr.type = j.value("type", "");
        qr.affected_rows = j.value("affected_rows", 0);

        // Если сервер подтвердил смену базы (USE или DROP), запоминаем это
        if (j.contains("current_db")) {
            current_db_ = j["current_db"].get<std::string>();
            qr.current_db = current_db_;
        }
        if (j.contains("current_user")) {
            current_user_ = j["current_user"].get<std::string>();
            qr.current_user = current_user_;
        }
        if (j.contains("session_id")) {
            session_id_ = j["session_id"].get<std::string>();
        }

        if (j.contains("columns")) {
            for (const auto& c : j["columns"])
                qr.columns.push_back(c.get<std::string>());
        }
        if (j.contains("rows")) {
            for (const auto& row : j["rows"]) {
                std::vector<std::string> r;
                for (const auto& v : row)
                    r.push_back(v.is_string() ? v.get<std::string>() : v.dump());
                qr.rows.push_back(std::move(r));
            }
        }
    } catch (const std::exception& e) {
        qr.message = std::string("Error: ") + e.what();
    }

    return qr;
}

QueryResult DBClient::executeText2Sql(const std::string& request) {
    QueryResult qr;
    qr.success = false;

    if (!connected_) {
        qr.message = "Not connected to server.";
        return qr;
    }

    try {
        httplib::Client cli(host_, port_);
        cli.set_connection_timeout(10);
        cli.set_read_timeout(120); // 120 seconds for LLM generation
        nlohmann::json body = {
            {"request", request},
            {"current_db", current_db_},
            {"current_user", current_user_},
            {"session_id", session_id_}
        };

        auto res = cli.Post("/text2sql", body.dump(), "application/json");
        if (!res) {
            qr.message = "Connection failed.";
            return qr;
        }

        auto j = nlohmann::json::parse(res->body);
        qr.success = j.value("success", false);
        qr.message = j.value("message", "");
        
        if (qr.success && j.contains("sql")) {
            qr.message = j["sql"].get<std::string>(); // Reusing message field to return SQL
        }
    } catch (const std::exception& e) {
        qr.message = std::string("Error: ") + e.what();
    }

    return qr;
}

} // namespace db
