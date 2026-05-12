#include "client_lib/db_client.hpp"
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <iostream>
#include <string>
#include <algorithm>
#include <iomanip>
#include <cstdlib>
#include <sstream>
#include <fstream>

// ── ASCII table formatting ─────────────────────────────────────────────

// Helper to format string if it's a number
static std::string formatValue(const std::string& val) {
    if (val.empty()) return val;
    try {
        size_t pos = 0;
        double d = std::stod(val, &pos);
        if (pos == val.size()) { // Pure number
            std::stringstream ss;
            ss << std::fixed << std::setprecision(3) << d;
            return ss.str();
        }
    } catch (...) {}
    return val;
}

static void printTable(const std::vector<std::string>& columns,
                       const std::vector<std::vector<std::string>>& rows) {
    std::vector<size_t> widths(columns.size());
    
    // Pre-format rows to calculate widths correctly
    std::vector<std::vector<std::string>> formatted_rows = rows;
    for (auto& row : formatted_rows) {
        for (auto& cell : row) {
            cell = formatValue(cell);
        }
    }

    for (size_t i = 0; i < columns.size(); ++i)
        widths[i] = columns[i].size();

    for (const auto& row : formatted_rows)
        for (size_t i = 0; i < row.size() && i < widths.size(); ++i)
            widths[i] = std::max(widths[i], row[i].size());

    auto printBorder = [&]() {
        std::cout << "+";
        for (size_t w : widths)
            std::cout << std::string(w + 2, '-') << "+";
        std::cout << "\n";
    };

    printBorder();

    std::cout << "|";
    for (size_t i = 0; i < columns.size(); ++i)
        std::cout << " " << std::left << std::setw(static_cast<int>(widths[i]))
                  << columns[i] << " |";
    std::cout << "\n";

    printBorder();

    for (const auto& row : formatted_rows) {
        std::cout << "|";
        for (size_t i = 0; i < columns.size(); ++i) {
            std::string val = (i < row.size()) ? row[i] : "";
            std::cout << " " << std::left << std::setw(static_cast<int>(widths[i]))
                      << val << " |";
        }
        std::cout << "\n";
    }

    printBorder();
    std::cout << rows.size() << " row(s) in set.\n";
}

static bool startsWith(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

static std::string trim(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.pop_back();
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start])))
        start++;
    return start > 0 ? s.substr(start) : s;
}

static std::string firstWordUpper(const std::string& s) {
    std::string first_word;
    for (unsigned char c : s) {
        if (c == ' ' || c == ';' || c == '\n' || c == '\t') break;
        first_word += static_cast<char>(std::toupper(c));
    }
    return first_word;
}

static bool isSqlStart(const std::string& first_word) {
    return first_word == "SELECT" || first_word == "CREATE" || first_word == "DROP" ||
           first_word == "INSERT" || first_word == "UPDATE" || first_word == "DELETE" ||
           first_word == "USE" || first_word == "ALTER" || first_word == "GRANT" ||
           first_word == "REVOKE" || first_word == "SET" || first_word == "SHOW" ||
           first_word == "LOAD" || first_word == "BEGIN" || first_word == "COMMIT" ||
           first_word == "ROLLBACK";
}

static std::string buildSchemaContext(db::DBClient& client, std::string& error) {
    auto tables = client.executeQuery("SHOW TABLES;");
    if (!tables.success) {
        error = "Не удалось получить схему БД: " + tables.message;
        return {};
    }
    if (tables.rows.empty()) {
        error = "Отсутствует информация о таблицах: в текущей базе нет таблиц.";
        return {};
    }

    std::ostringstream schema;
    for (const auto& row : tables.rows) {
        if (row.empty()) continue;
        const std::string table = row[0];
        auto cols = client.executeQuery("SHOW COLUMNS FROM " + table + ";");
        if (!cols.success) {
            error = "Не удалось получить схему таблицы '" + table + "': " + cols.message;
            return {};
        }

        schema << "TABLE " << table << " (";
        for (size_t i = 0; i < cols.rows.size(); ++i) {
            const auto& c = cols.rows[i];
            if (c.size() < 2) continue;
            if (i > 0) schema << ", ";
            schema << c[0] << " " << c[1];
            if (c.size() > 2 && c[2] == "NO") schema << " NOT NULL";
            if (c.size() > 3 && c[3] == "PRI") schema << " PRIMARY KEY";
            else if (c.size() > 3 && c[3] == "UNI") schema << " UNIQUE";
        }
        schema << ")\n";
    }
    return schema.str();
}

static std::string extractJsonObject(const std::string& text) {
    size_t begin = text.find('{');
    size_t end = text.rfind('}');
    if (begin == std::string::npos || end == std::string::npos || begin > end)
        return {};
    return text.substr(begin, end - begin + 1);
}

static std::string readApiKeyFromFile(const std::string& path) {
    std::ifstream in(path);
    if (!in) return {};
    std::string key;
    std::getline(in, key);
    return trim(key);
}

static bool isPlaceholderApiKey(const std::string& key) {
    return key.empty() || key == "put-your-mistral-api-key-here" ||
           key.find("PASTE_") != std::string::npos ||
           key.find("YOUR_") != std::string::npos;
}

static void ensureMistralApiKeyFile(const std::string& path) {
    std::ifstream existing(path);
    if (existing.good()) return;

    std::ofstream out(path);
    if (!out) return;
    out << "put-your-mistral-api-key-here\n";
}

static std::string loadMistralApiKey() {
    if (const char* env_key = std::getenv("MISTRAL_API_KEY")) {
        std::string key = trim(env_key);
        if (!isPlaceholderApiKey(key)) return key;
    }

    if (const char* env_file = std::getenv("MISTRAL_API_KEY_FILE")) {
        std::string key = readApiKeyFromFile(env_file);
        if (!isPlaceholderApiKey(key)) return key;
    }

    constexpr const char* canonical_key_file = "mistral_api_key";
    std::string key = readApiKeyFromFile(canonical_key_file);
    if (!isPlaceholderApiKey(key)) return key;

    // Backward compatibility with earlier local names. If a real key is found,
    // migrate it into the new canonical file name without printing it.
    for (const char* legacy_path : {".mistral_api_key", ".mistral_api_key.example"}) {
        key = readApiKeyFromFile(legacy_path);
        if (!isPlaceholderApiKey(key)) {
            std::ofstream out(canonical_key_file);
            if (out) out << key << "\n";
            return key;
        }
    }

    ensureMistralApiKeyFile(canonical_key_file);
    return {};
}

static bool generateSqlWithMistral(const std::string& ru_request,
                                   const std::string& schema_context,
                                   std::string& sql,
                                   std::string& message) {
    std::string api_key = loadMistralApiKey();
    if (api_key.empty()) {
        message = "API ключ Mistral не задан. Установите MISTRAL_API_KEY, MISTRAL_API_KEY_FILE "
                  "или заполните локальный файл mistral_api_key.";
        return false;
    }
    std::string model = "mistral-small-latest";
    if (const char* env_model = std::getenv("MISTRAL_MODEL")) {
        if (*env_model) model = env_model;
    }

    nlohmann::json body;
    body["model"] = model;
    body["temperature"] = 0.0;
    body["messages"] = nlohmann::json::array({
        {
            {"role", "system"},
            {"content",
             "Ты переводишь точные русскоязычные запросы пользователя в SQL для учебной СУБД. "
             "Используй только переданную схему. Не выдумывай таблицы, колонки, значения и условия. "
             "Если для корректного SQL не хватает таблицы, колонки, условия, периода, значения или другой "
             "обязательной информации, верни JSON: {\"success\":false,\"message\":\"Отсутствует информация о ...\"}. "
             "Если информации достаточно, верни только JSON: {\"success\":true,\"sql\":\"...\"}. "
             "SQL должен быть одним запросом без markdown и без пояснений."}
        },
        {
            {"role", "user"},
            {"content", "Схема базы данных:\n" + schema_context + "\nЗадача на русском:\n" + ru_request}
        }
    });

    httplib::SSLClient cli("api.mistral.ai", 443);
    cli.set_connection_timeout(10);
    cli.set_read_timeout(60);

    httplib::Headers headers = {
        {"Authorization", "Bearer " + api_key},
        {"Content-Type", "application/json"}
    };
    auto res = cli.Post("/v1/chat/completions", headers, body.dump(), "application/json");
    if (!res) {
        message = "Не удалось подключиться к Mistral API.";
        return false;
    }
    if (res->status < 200 || res->status >= 300) {
        message = "Mistral API вернул HTTP " + std::to_string(res->status) + ": " + res->body;
        return false;
    }

    try {
        auto response = nlohmann::json::parse(res->body);
        std::string content = response["choices"][0]["message"]["content"].get<std::string>();
        auto parsed = nlohmann::json::parse(extractJsonObject(content));
        if (!parsed.value("success", false)) {
            message = parsed.value("message", "Отсутствует информация о задаче.");
            return false;
        }
        sql = trim(parsed.value("sql", ""));
        if (sql.empty()) {
            message = "Mistral API вернул пустой SQL.";
            return false;
        }
        return true;
    } catch (const std::exception& e) {
        message = std::string("Не удалось разобрать ответ Mistral API: ") + e.what();
        return false;
    }
}

static bool handleText2Sql(db::DBClient& client, const std::string& request) {
    std::string schema_error;
    std::string schema = buildSchemaContext(client, schema_error);
    if (schema.empty()) {
        std::cout << "\033[31m[TEXT2SQL]\033[0m " << schema_error << "\n\n";
        return true;
    }

    std::string sql;
    std::string msg;
    if (!generateSqlWithMistral(request, schema, sql, msg)) {
        std::cout << "\033[31m[TEXT2SQL]\033[0m " << msg << "\n\n";
        return true;
    }

    std::cout << "\033[36m[TEXT2SQL]\033[0m " << sql << "\n";
    auto result = client.executeQuery(sql);
    if (!result.success) {
        std::cout << "\033[31m[ERROR]\033[0m " << result.message << "\n\n";
        return true;
    }
    if (!result.columns.empty()) {
        printTable(result.columns, result.rows);
    } else if (!result.message.empty()) {
        std::cout << "\033[32m[OK]\033[0m " << result.message << "\n";
    }
    std::cout << "\n";
    return true;
}

// ── Main REPL ──────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    std::string host = "127.0.0.1";
    int port = 8080;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--host" && i + 1 < argc) host = argv[++i];
        else if (arg == "--port" && i + 1 < argc) port = std::stoi(argv[++i]);
    }

    db::DBClient client;
    if (!client.connect(host, port)) {
        std::cerr << "[!] Cannot connect to server at " << host << ":" << port << "\n";
        return 1;
    }

    std::cout << "╔══════════════════════════════════════╗\n";
    std::cout << "║       databasetopit CLI v1.0         ║\n";
    std::cout << "║  Connected to " << host << ":" << port;
    int pad = 22 - static_cast<int>(host.size()) - static_cast<int>(std::to_string(port).size());
    for (int i = 0; i < pad; ++i) std::cout << " ";
    std::cout << "║\n";
    std::cout << "║  Type SQL, \\ai text, or 'exit'       ║\n";
    std::cout << "╚══════════════════════════════════════╝\n\n";

    std::string line;
    std::string query;

    std::string prompt_user = "admin";
    std::string prompt_db = "(none)";

    while (true) {
        std::string sout = "\033[1;36m" + prompt_user + "@" + prompt_db + "\033[0m> ";
        std::cout << (query.empty() ? sout : "  -> ");

        if (!std::getline(std::cin, line)) break;

        line = trim(line);

        if (line == "exit" || line == "quit" || line == "\\q") break;
        if (line.empty()) continue;

        if (line == "\\c") {
            query.clear();
            std::cout << "Query buffer cleared.\n\n";
            continue;
        }

        if (query.empty() && (startsWith(line, "\\ai ") || startsWith(line, "\\text2sql "))) {
            std::string request = startsWith(line, "\\ai ") ? line.substr(4) : line.substr(10);
            handleText2Sql(client, trim(request));
            continue;
        }

        query += (query.empty() ? "" : " ") + line;

        std::string first_word = firstWordUpper(query);

        if (!first_word.empty()) {
            if (!isSqlStart(first_word)) {
                handleText2Sql(client, query);
                query.clear();
                continue;
            }
        }

        bool has_semi = false;
        for (auto it = query.rbegin(); it != query.rend(); ++it) {
            if (*it == ' ' || *it == '\t' || *it == '\r' || *it == '\n') continue;
            if (*it == ';') has_semi = true;
            break;
        }

        auto result = client.executeQuery(query, !has_semi);

        if (!has_semi) {
            // During multi-line entry, we don't show errors to avoid interrupting the user
            continue;
        }

        query.clear();

        if (!result.success) {
            std::cout << "\033[31m[ERROR]\033[0m " << result.message << "\n\n";
            continue;
        }

        if (!result.columns.empty()) {
            printTable(result.columns, result.rows);
        } else if (result.type == "modify" || result.success) {
            std::string msg = result.message;
            if (msg.find("Using database '") == 0) {
                size_t start = 16;
                size_t end = msg.find("'", start);
                if (end != std::string::npos) {
                    prompt_db = "(" + msg.substr(start, end - start) + ")";
                }
            } else if (msg.find("Context switched to user: ") == 0) {
                prompt_user = msg.substr(26);
            }
            if (!msg.empty() && result.type != "select") {
                std::cout << "\033[32m[OK]\033[0m " << msg;
            }
        }
        std::cout << "\n";
    }

    std::cout << "Goool nakonec-to ti vishel!\n";
    return 0;
}
