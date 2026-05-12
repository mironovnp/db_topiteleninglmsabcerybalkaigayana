#include "server/server.hpp"
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <iostream>
#include <algorithm>
#include <cctype>

namespace db {

using json = nlohmann::json;

Server::Server(const std::string& host, int port, const std::string& data_dir)
    : host_(host), port_(port), executor_(data_dir) {
    svr_ptr_ = new httplib::Server();
}

void Server::start() {
    auto& svr = *static_cast<httplib::Server*>(svr_ptr_);

    svr.Post("/query", [this](const httplib::Request& req, httplib::Response& res) {
        try {
            auto body = json::parse(req.body);

            std::string session_id = body.value("session_id", "");
            SessionContext session;
            {
                std::lock_guard<std::mutex> lock(sessions_mutex_);
                if (session_id.empty()) {
                    session_id = "s" + std::to_string(next_session_id_.fetch_add(1));
                    SessionContext fresh;
                    fresh.current_db = body.value("current_db", "");
                    sessions_[session_id] = fresh;
                }
                auto it = sessions_.find(session_id);
                if (it == sessions_.end()) {
                    it = sessions_.emplace(session_id, SessionContext{}).first;
                }
                session = it->second;
            }

            // Thread-local хранит только рабочий контекст текущего HTTP-потока.
            // Пользователь берется из серверной сессии, а не из тела запроса.
            executor_.setThreadLocalContext(session.current_db);
            executor_.setThreadLocalUser(session.current_user);

            auto attach_session_context = [&](json result) {
                std::lock_guard<std::mutex> lock(sessions_mutex_);
                auto& stored = sessions_[session_id];
                if (result.contains("current_db")) {
                    stored.current_db = result.value("current_db", "");
                }
                if (result.contains("current_user")) {
                    stored.current_user = result.value("current_user", "");
                }
                result["session_id"] = session_id;
                result["current_db"] = stored.current_db;
                result["current_user"] = stored.current_user;
                return result;
            };

            std::string sql = body.value("sql", "");
            if (sql.empty()) {
                res.set_content(
                    attach_session_context(json({{"success", false}, {"message", "Empty query"}})).dump(),
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
                        res.set_content(
                            attach_session_context(json({{"success", true}, {"type", "incomplete"}})).dump(),
                            "application/json");
                    } else {
                        res.set_content(
                            attach_session_context(json({{"success", false}, {"message", err}})).dump(),
                            "application/json");
                    }
                    return;
                }
                res.set_content(
                    attach_session_context(json({{"success", true}, {"type", "incomplete"}})).dump(),
                    "application/json");
                return;
            }

            // Определяем, является ли запрос безопасным для параллельного чтения
            std::string upper_sql = sql;
            upper_sql.erase(upper_sql.begin(), std::find_if(upper_sql.begin(), upper_sql.end(), [](unsigned char ch) {
                return !std::isspace(ch);
            }));
            std::transform(upper_sql.begin(), upper_sql.end(), upper_sql.begin(), ::toupper);

            bool is_read_only = (upper_sql.find("SELECT") == 0 || upper_sql.find("SHOW") == 0);

            nlohmann::json result;

            // БЛОК МНОГОПОТОЧНОЙ СИНХРОНИЗАЦИИ
            if (is_read_only) {
                std::shared_lock<std::shared_mutex> lock(db_rw_mutex_);
                result = executor_.execute(sql);
            } else {
                std::unique_lock<std::shared_mutex> lock(db_rw_mutex_);
                result = executor_.execute(sql);
            }

            res.set_content(attach_session_context(result).dump(), "application/json");
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

void Server::stop() {
    if (svr_ptr_) {
        static_cast<httplib::Server*>(svr_ptr_)->stop();
    }
}

} // namespace db
