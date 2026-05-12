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



            std::string sql = body.value("sql", "");
            if (sql.empty()) {
                res.set_content(
                    attach_session_context(session_id, json({{"success", false}, {"message", "Empty query"}})).dump(),
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
                            attach_session_context(session_id, json({{"success", true}, {"type", "incomplete"}})).dump(),
                            "application/json");
                    } else {
                        res.set_content(
                            attach_session_context(session_id, json({{"success", false}, {"message", err}})).dump(),
                            "application/json");
                    }
                    return;
                }
                res.set_content(
                    attach_session_context(session_id, json({{"success", true}, {"type", "incomplete"}})).dump(),
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

            res.set_content(attach_session_context(session_id, result).dump(), "application/json");
        } catch (const std::exception& e) {
            res.set_content(
                json({{"success", false}, {"message", e.what()}}).dump(),
                "application/json");
        }
    });

    svr.Post("/text2sql", [this](const httplib::Request& req, httplib::Response& res) {
        try {
            auto body = json::parse(req.body);
            std::string request = body.value("request", "");
            std::string session_id = body.value("session_id", "");
            
            SessionContext session;
            {
                std::lock_guard<std::mutex> lock(sessions_mutex_);
                auto it = sessions_.find(session_id);
                if (it != sessions_.end()) session = it->second;
            }

            executor_.setThreadLocalContext(session.current_db);
            executor_.setThreadLocalUser(session.current_user);

            // 1. Build schema context
            std::string schema;
            auto tables_res = executor_.execute("SHOW TABLES;");
            if (tables_res.value("success", false) && tables_res.contains("rows")) {
                for (const auto& row : tables_res["rows"]) {
                    if (row.empty()) continue;
                    std::string table = row[0].get<std::string>();
                    schema += "TABLE " + table + " (";
                    auto cols_res = executor_.execute("SHOW COLUMNS FROM " + table + ";");
                    if (cols_res.value("success", false) && cols_res.contains("rows")) {
                        for (size_t i = 0; i < cols_res["rows"].size(); ++i) {
                            const auto& c = cols_res["rows"][i];
                            if (c.size() < 2) continue;
                            if (i > 0) schema += ", ";
                            schema += c[0].get<std::string>() + " " + c[1].get<std::string>();
                        }
                    }
                    schema += ")\n";
                }
            }

            if (schema.empty()) {
                res.set_content(json({{"success", false}, {"message", "Database is empty, no schema available"}}).dump(), "application/json");
                return;
            }

            // 2. Call Gemini
            std::string api_key = "AIzaSyCiBT7cfpt4gtD0Ki_UJloqGU773JB_urQ";

            httplib::SSLClient cli("generativelanguage.googleapis.com", 443);
            cli.set_connection_timeout(30);
            cli.set_read_timeout(120);

            json gemini_body;
            gemini_body["contents"] = json::array({{
                {"parts", json::array({{
                    {"text", 
                        "Ты — эксперт SQL для базы данных databasetopit. Переведи запрос пользователя в SQL. "
                        "Используй только предоставленную схему. Возвращай строго JSON: {\"success\":true, \"sql\":\"...\"} или {\"success\":false, \"message\":\"...\"}. "
                        "Не добавляй markdown или текст вне JSON.\n"
                        "Схема:\n" + schema + "\nЗапрос:\n" + request
                    }
                }})}
            }});
            gemini_body["generationConfig"]["temperature"] = 0.0;
            gemini_body["generationConfig"]["responseMimeType"] = "application/json";

            auto gemini_res = cli.Post("/v1beta/models/gemini-1.5-flash:generateContent?key=" + api_key, 
                                      gemini_body.dump(), "application/json");
            
            if (!gemini_res) {
                res.set_content(json({{"success", false}, {"message", "Failed to connect to Gemini API"}}).dump(), "application/json");
                return;
            }

            if (gemini_res->status != 200) {
                try {
                    auto err_json = json::parse(gemini_res->body);
                    if (err_json.contains("error")) {
                        res.set_content(json({{"success", false}, {"message", "Gemini API error: " + err_json["error"].value("message", "unknown")}}).dump(), "application/json");
                        return;
                    }
                } catch (...) {}
                res.set_content(json({{"success", false}, {"message", "Gemini API returned status " + std::to_string(gemini_res->status)}}).dump(), "application/json");
                return;
            }

            auto gemini_json = json::parse(gemini_res->body);
            if (!gemini_json.contains("candidates") || gemini_json["candidates"].empty()) {
                std::string extra;
                if (gemini_json.contains("promptFeedback")) extra = " (Prompt blocked)";
                res.set_content(json({{"success", false}, {"message", "Gemini returned no candidates" + extra}}).dump(), "application/json");
                return;
            }
            
            std::string content = gemini_json["candidates"][0]["content"]["parts"][0]["text"].get<std::string>();
            
            // Clean up Gemini output (sometimes it wraps in markdown)
            size_t start = content.find('{');
            size_t end = content.rfind('}');
            if (start != std::string::npos && end != std::string::npos && start < end) {
                content = content.substr(start, end - start + 1);
            }

            res.set_content(attach_session_context(session_id, json::parse(content)).dump(), "application/json");

        } catch (const std::exception& e) {
            res.set_content(json({{"success", false}, {"message", e.what()}}).dump(), "application/json");
        }
    });

    svr.Post("/auth", [this](const httplib::Request& req, httplib::Response& res) {
        try {
            auto body = json::parse(req.body);
            std::string action = body.value("action", "");   // "login" or "register"
            std::string username = body.value("username", "");
            std::string password = body.value("password", "");
            std::string session_id = body.value("session_id", "");

            if (action.empty() || username.empty() || password.empty()) {
                res.set_content(json({{"success", false}, {"message", "Missing action, username, or password"}}).dump(),
                                "application/json");
                return;
            }

            // Generate session if needed
            {
                std::lock_guard<std::mutex> lock(sessions_mutex_);
                if (session_id.empty()) {
                    session_id = "s" + std::to_string(next_session_id_.fetch_add(1));
                    sessions_[session_id] = SessionContext{};
                }
            }

            // Build SQL from action
            std::string sql;
            if (action == "login") {
                sql = "LOGIN " + username + " PASSWORD '" + password + "';";
            } else if (action == "register") {
                sql = "REGISTER " + username + " PASSWORD '" + password + "';";
            } else {
                res.set_content(json({{"success", false}, {"message", "Unknown action: " + action}}).dump(),
                                "application/json");
                return;
            }

            executor_.setThreadLocalContext("");
            executor_.setThreadLocalUser("");

            nlohmann::json result;
            {
                std::unique_lock<std::shared_mutex> lock(db_rw_mutex_);
                result = executor_.execute(sql);
            }

            res.set_content(attach_session_context(session_id, result).dump(), "application/json");
        } catch (const std::exception& e) {
            res.set_content(json({{"success", false}, {"message", e.what()}}).dump(), "application/json");
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

json Server::attach_session_context(const std::string& session_id, json result) {
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
}

} // namespace db
