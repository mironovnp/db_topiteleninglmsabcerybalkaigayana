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
           first_word == "ROLLBACK" || first_word == "REGISTER" || first_word == "LOGIN" ||
           first_word == "LOGOUT" || first_word == "CHANGE";
}

static bool handleText2Sql(db::DBClient& client, const std::string& request, std::string& prompt_db, std::string& prompt_user) {
    auto ai_res = client.executeText2Sql(request);
    if (!ai_res.success) {
        std::cout << "\033[31m[TEXT2SQL]\033[0m " << ai_res.message << "\n\n";
        return true;
    }

    std::string sql = ai_res.message;

    std::cout << "\033[36m[TEXT2SQL]\033[0m " << sql << "\n";
    auto result = client.executeQuery(sql);
    if (!result.success) {
        std::cout << "\033[31m[ERROR]\033[0m " << result.message << "\n\n";
        return true;
    }

    if (!result.current_db.empty()) {
        prompt_db = "(" + result.current_db + ")";
    } else {
        prompt_db = "(none)";
    }
    if (!result.current_user.empty()) {
        prompt_user = result.current_user;
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
    std::string prompt_user = "";
    std::string prompt_db = "(none)";

    while (true) {
        // ═══ AUTH FLOW ═══
        while (prompt_user.empty()) {
            std::cout << "\033[1;33mLogin or Register? (l/r):\033[0m ";
            if (!std::getline(std::cin, line)) return 0;
            line = trim(line);
            if (line == "exit" || line == "quit") return 0;

            bool is_login = (line == "l" || line == "L" || line == "login");
            bool is_register = (line == "r" || line == "R" || line == "register");
            if (!is_login && !is_register) {
                std::cout << "\033[31mPlease enter 'l' for login or 'r' for register.\033[0m\n";
                continue;
            }

            std::string username, password;
            std::cout << "Username: ";
            if (!std::getline(std::cin, username)) return 0;
            username = trim(username);
            if (username.empty()) { std::cout << "\033[31mUsername cannot be empty.\033[0m\n"; continue; }

            std::cout << "Password: ";
            if (!std::getline(std::cin, password)) return 0;
            password = trim(password);
            if (password.empty()) { std::cout << "\033[31mPassword cannot be empty.\033[0m\n"; continue; }

            std::string sql = is_login 
                ? "LOGIN " + username + " PASSWORD '" + password + "';"
                : "REGISTER " + username + " PASSWORD '" + password + "';";

            auto result = client.executeQuery(sql);
            if (result.success) {
                prompt_user = result.current_user.empty() ? username : result.current_user;
                std::cout << "\033[32m[OK]\033[0m " << result.message << "\n\n";
            } else {
                std::cout << "\033[31m[ERROR]\033[0m " << result.message << "\n\n";
            }
        }

        while (!prompt_user.empty()) {
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
            handleText2Sql(client, trim(request), prompt_db, prompt_user);
            continue;
        }

        query += (query.empty() ? "" : " ") + line;

        std::string first_word = firstWordUpper(query);

        if (!first_word.empty()) {
            if (!isSqlStart(first_word)) {
                handleText2Sql(client, query, prompt_db, prompt_user);
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
        } else if (result.success) {
            if (!result.current_db.empty()) {
                prompt_db = "(" + result.current_db + ")";
            } else {
                prompt_db = "(none)";
            }
            if (!result.current_user.empty()) {
                prompt_user = result.current_user;
            }

            std::string msg = result.message;
            if (!msg.empty() && result.type != "select") {
                std::cout << "\033[32m[OK]\033[0m " << msg;
            }
            if (result.type == "logout") {
                prompt_user.clear();
                prompt_db = "(none)";
            }
        }
        std::cout << "\n";
    }
    }

    std::cout << "Goool nakonec-to ti vishel!\n";
    return 0;
}
