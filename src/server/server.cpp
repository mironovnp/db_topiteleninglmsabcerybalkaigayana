#include "server/server.hpp"
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <iostream>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace {

std::string sanitize_csv_filename(std::string name) {
    const auto pos = name.find_last_of("/\\");
    if (pos != std::string::npos) name = name.substr(pos + 1);
    if (name.empty()) name = "import.csv";
    std::string out;
    out.reserve(name.size());
    for (unsigned char c : name) {
        if (std::isalnum(c) || c == '_' || c == '-' || c == '.')
            out.push_back(static_cast<char>(c));
    }
    if (out.empty()) out = "import.csv";
    if (out.find('.') == std::string::npos) out += ".csv";
    return out;
}

bool is_safe_identifier(const std::string& name) {
    if (name.empty()) return false;
    for (size_t i = 0; i < name.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(name[i]);
        if (i == 0) {
            if (!std::isalpha(c) && c != '_') return false;
        } else if (!std::isalnum(c) && c != '_') {
            return false;
        }
    }
    return true;
}

std::string escape_sql_string(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '\'') out += "''";
        else out.push_back(c);
    }
    return out;
}

static std::string trim_field(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    return s;
}

// Query (?table=…) попадает в req.params; части multipart — в req.files (не в params).
std::string multipart_field(const httplib::Request& req, const std::string& name) {
    if (req.has_param(name)) {
        const auto v = trim_field(req.get_param_value(name));
        if (!v.empty()) return v;
    }
    if (req.has_file(name)) {
        const auto v = trim_field(req.get_file_value(name).content);
        if (!v.empty()) return v;
    }
    for (const auto& kv : req.files) {
        if (kv.second.name == name) {
            const auto v = trim_field(kv.second.content);
            if (!v.empty()) return v;
        }
    }
    return "";
}

} // namespace

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
                if (body.contains("current_db")) {
                    const std::string req_db = body.value("current_db", "");
                    if (!req_db.empty())
                        it->second.current_db = req_db;
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

            // 2. Call Mistral AI (key must not live in the binary; set MISTRAL_API_KEY in the environment)
            const char* key_env = std::getenv("MISTRAL_API_KEY");
            std::string api_key = key_env ? std::string(key_env) : std::string();
            if (api_key.empty()) {
                res.set_content(
                    json({{"success", false},
                          {"message", "text2sql is disabled: set the MISTRAL_API_KEY environment variable."}})
                        .dump(),
                    "application/json");
                return;
            }

            httplib::SSLClient cli("api.mistral.ai");
            cli.set_connection_timeout(10); // Increase timeout
            cli.set_read_timeout(20);
            cli.enable_server_certificate_verification(true);

            json mistral_body;
            mistral_body["model"] = "mistral-small-latest";
            mistral_body["messages"] = json::array({
                {{"role", "user"}, {"content", 
                    "Ты — эксперт SQL для базы данных databasetopit. Переведи запрос пользователя в SQL. "
                    "Используй только предоставленную схему. Возвращай строго JSON: {\"success\":true, \"sql\":\"...\"} или {\"success\":false, \"message\":\"...\"}. "
                    "Не добавляй markdown или текст вне JSON.\n"
                    "Схема:\n" + schema + "\nЗапрос:\n" + request
                }}
            });
            mistral_body["response_format"] = {{"type", "json_object"}};
            mistral_body["temperature"] = 0.0;

            httplib::Headers headers = {
                {"Authorization", "Bearer " + api_key}
            };

            auto mistral_res = cli.Post("/v1/chat/completions", headers, mistral_body.dump(), "application/json");
            
            if (!mistral_res) {
                res.set_content(json({{"success", false}, {"message", "Failed to connect to Mistral API"}}).dump(), "application/json");
                return;
            }

            if (mistral_res->status != 200) {
                try {
                    auto err_json = json::parse(mistral_res->body);
                    if (err_json.contains("error")) {
                        res.set_content(json({{"success", false}, {"message", "Mistral API error: " + err_json["error"].value("message", "unknown")}}).dump(), "application/json");
                        return;
                    }
                } catch (...) {}
                res.set_content(json({{"success", false}, {"message", "Mistral API returned status " + std::to_string(mistral_res->status)}}).dump(), "application/json");
                return;
            }

            auto mistral_json = json::parse(mistral_res->body);
            if (!mistral_json.contains("choices") || mistral_json["choices"].empty()) {
                res.set_content(json({{"success", false}, {"message", "Mistral returned no choices"}}).dump(), "application/json");
                return;
            }
            
            std::string content = mistral_json["choices"][0]["message"]["content"].get<std::string>();
            
            // Clean up Mistral output (just in case, although response_format is used)
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

    svr.Post("/import-csv", [this](const httplib::Request& req, httplib::Response& res) {
        try {
            if (!req.is_multipart_form_data()) {
                res.set_content(
                    json({{"success", false}, {"message", "Expected multipart/form-data with file field."}})
                        .dump(),
                    "application/json");
                return;
            }

            if (!req.has_file("file")) {
                res.set_content(
                    json({{"success", false}, {"message", "Missing file field 'file'."}}).dump(),
                    "application/json");
                return;
            }

            const auto& upload = req.get_file_value("file");
            if (upload.content.empty()) {
                res.set_content(
                    json({{"success", false}, {"message", "CSV file is empty."}}).dump(),
                    "application/json");
                return;
            }

            constexpr size_t kMaxCsvBytes = 32 * 1024 * 1024;
            if (upload.content.size() > kMaxCsvBytes) {
                res.set_content(
                    json({{"success", false},
                          {"message", "CSV file is too large (max 32 MB)."}})
                        .dump(),
                    "application/json");
                return;
            }

            std::string session_id = multipart_field(req, "session_id");
            std::string database = multipart_field(req, "database");
            std::string table = multipart_field(req, "table");

            bool append = false;
            {
                const std::string a = multipart_field(req, "append");
                append = (a == "1" || a == "true" || a == "TRUE" || a == "yes");
            }

            if (table.empty()) {
                res.set_content(
                    json({{"success", false},
                          {"message", "Не указана таблица для импорта (поле table)."}})
                        .dump(),
                    "application/json");
                return;
            }
            if (!is_safe_identifier(table)) {
                res.set_content(
                    json({{"success", false}, {"message", "Invalid table name."}}).dump(),
                    "application/json");
                return;
            }

            SessionContext session;
            {
                std::lock_guard<std::mutex> lock(sessions_mutex_);
                if (session_id.empty()) {
                    session_id = "s" + std::to_string(next_session_id_.fetch_add(1));
                    sessions_[session_id] = SessionContext{};
                }
                auto it = sessions_.find(session_id);
                if (it == sessions_.end()) {
                    it = sessions_.emplace(session_id, SessionContext{}).first;
                }
                session = it->second;
            }

            if (!database.empty()) session.current_db = database;
            if (session.current_db.empty()) {
                res.set_content(
                    json({{"success", false},
                          {"message", "No database selected. Choose a database in the toolbar or pass 'database'."}})
                        .dump(),
                    "application/json");
                return;
            }
            if (session.current_user.empty()) {
                res.set_content(
                    json({{"success", false}, {"message", "Not authenticated."}}).dump(),
                    "application/json");
                return;
            }

            const std::string server_filename = sanitize_csv_filename(upload.filename);
            const auto db_dir = executor_.databaseDirectory(session.current_db);
            std::error_code ec;
            std::filesystem::create_directories(db_dir, ec);
            const auto dest = db_dir / server_filename;

            {
                std::ofstream out(dest, std::ios::binary | std::ios::trunc);
                if (!out) {
                    res.set_content(
                        json({{"success", false}, {"message", "Failed to write CSV on server."}}).dump(),
                        "application/json");
                    return;
                }
                out.write(upload.content.data(), static_cast<std::streamsize>(upload.content.size()));
                if (!out) {
                    res.set_content(
                        json({{"success", false}, {"message", "Failed to write CSV on server."}}).dump(),
                        "application/json");
                    return;
                }
            }

            executor_.setThreadLocalContext(session.current_db);
            executor_.setThreadLocalUser(session.current_user);

            std::string sql = "LOAD CSV '" + escape_sql_string(server_filename) + "' INTO " + table;
            if (append) sql += " APPEND";
            sql += ";";

            json result;
            {
                std::unique_lock<std::shared_mutex> lock(db_rw_mutex_);
                result = executor_.execute(sql);
            }

            result["server_filename"] = server_filename;
            result["sql"] = sql;
            res.set_content(attach_session_context(session_id, result).dump(), "application/json");
        } catch (const std::exception& e) {
            res.set_content(
                json({{"success", false}, {"message", e.what()}}).dump(),
                "application/json");
        }
    });

    svr.Get("/ping", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(
            json({{"status", "ok"}, {"import_csv_v2", true}}).dump(),
            "application/json");
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
    if (!stored.current_user.empty()) {
        executor_.setThreadLocalUser(stored.current_user);
        result["is_admin"] = executor_.isAdmin();
    }
    return result;
}

} // namespace db
