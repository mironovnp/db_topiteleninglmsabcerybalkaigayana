#include "client_lib/db_client.hpp"
#include <iostream>
#include <string>
#include <algorithm>
#include <iomanip>

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
    std::cout << "║  Type SQL queries or 'exit' to quit  ║\n";
    std::cout << "╚══════════════════════════════════════╝\n\n";

    std::string line;
    std::string query;

    std::string prompt_user = "admin";
    std::string prompt_db = "(none)";

    while (true) {
        std::string sout = "\033[1;36m" + prompt_user + "@" + prompt_db + "\033[0m> ";
        std::cout << (query.empty() ? sout : "  -> ");

        if (!std::getline(std::cin, line)) break;

        // Trim trailing whitespace
        while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back())))
            line.pop_back();
        // Trim leading whitespace
        size_t start = 0;
        while (start < line.size() && std::isspace(static_cast<unsigned char>(line[start])))
            start++;
        if (start > 0) line = line.substr(start);

        if (line == "exit" || line == "quit" || line == "\\q") break;
        if (line.empty()) continue;

        if (line == "\\c") {
            query.clear();
            std::cout << "Query buffer cleared.\n\n";
            continue;
        }

        query += (query.empty() ? "" : " ") + line;

        std::string first_word;
        for (char c : query) {
            if (c == ' ' || c == ';' || c == '\n' || c == '\t') break;
            first_word += std::toupper(c);
        }

        if (!first_word.empty()) {
            // ДОБАВЛЕНЫ КЛЮЧЕВЫЕ СЛОВА SHOW И LOAD
            bool valid_start = (first_word == "SELECT" || first_word == "CREATE" ||
                                first_word == "DROP" || first_word == "INSERT" ||
                                first_word == "UPDATE" || first_word == "DELETE" ||
                                first_word == "USE" || first_word == "ALTER" ||
                                first_word == "GRANT" || first_word == "REVOKE" ||
                                first_word == "SET" || first_word == "SHOW" ||
                                first_word == "LOAD");
            if (!valid_start) {
                std::cout << "\033[31m[ERROR]\033[0m Unexpected keyword: " << first_word << "\n\n";
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
