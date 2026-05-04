#include "client_lib/db_client.hpp"
#include <iostream>
#include <string>
#include <algorithm>
#include <iomanip>

// ── ASCII table formatting ─────────────────────────────────────────────

static void printTable(const std::vector<std::string>& columns,
                       const std::vector<std::vector<std::string>>& rows) {
    // Calculate column widths
    std::vector<size_t> widths(columns.size());
    for (size_t i = 0; i < columns.size(); ++i)
        widths[i] = columns[i].size();

    for (const auto& row : rows)
        for (size_t i = 0; i < row.size() && i < widths.size(); ++i)
            widths[i] = std::max(widths[i], row[i].size());

    // Top border
    auto printBorder = [&]() {
        std::cout << "+";
        for (size_t w : widths)
            std::cout << std::string(w + 2, '-') << "+";
        std::cout << "\n";
    };

    printBorder();

    // Header
    std::cout << "|";
    for (size_t i = 0; i < columns.size(); ++i)
        std::cout << " " << std::left << std::setw(static_cast<int>(widths[i]))
                  << columns[i] << " |";
    std::cout << "\n";

    printBorder();

    // Data rows
    for (const auto& row : rows) {
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
    // Pad to align
    int pad = 22 - static_cast<int>(host.size()) - static_cast<int>(std::to_string(port).size());
    for (int i = 0; i < pad; ++i) std::cout << " ";
    std::cout << "║\n";
    std::cout << "║  Type SQL queries or 'exit' to quit  ║\n";
    std::cout << "╚══════════════════════════════════════╝\n\n";

    std::string line;
    std::string query;

    while (true) {
        std::cout << (query.empty() ? "sql> " : "  -> ");

        if (!std::getline(std::cin, line)) break;

        // Trim
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
            line.pop_back();

        if (line == "exit" || line == "quit" || line == "\\q") break;
        if (line.empty()) continue;

        if (line == "\\c") {
            query.clear();
            std::cout << "Query buffer cleared.\n\n";
            continue;
        }

        query += (query.empty() ? "" : " ") + line;

        // Check if the first word is a valid SQL command to avoid buffering garbage
        std::string first_word;
        for (char c : query) {
            if (c == ' ' || c == ';' || c == '\n' || c == '\t') break;
            first_word += std::toupper(c);
        }
        
        if (!first_word.empty()) {
            bool valid_start = (first_word == "SELECT" || first_word == "CREATE" || 
                                first_word == "DROP" || first_word == "INSERT" || 
                                first_word == "UPDATE" || first_word == "DELETE" || 
                                first_word == "USE" || first_word == "ALTER");
            if (!valid_start) {
                std::cout << "\033[31m[ERROR]\033[0m Unexpected keyword: " << first_word << "\n\n";
                query.clear();
                continue;
            }
        }

        bool has_semi = (query.back() == ';');

        // If it doesn't have a semicolon, we validate syntax (dry_run)
        auto result = client.executeQuery(query, !has_semi);

        if (!has_semi) {
            if (!result.success) {
                // It's a REAL syntax error (not just EOF)
                std::cout << "\033[31m[ERROR]\033[0m " << result.message << "\n\n";
                query.clear();
            }
            // If success (type == "incomplete"), just show -> and wait for more
            continue;
        }

        query.clear();

        if (!result.success) {
            std::cout << "\033[31m[ERROR]\033[0m " << result.message << "\n\n";
            continue;
        }

        if (result.type == "select") {
            printTable(result.columns, result.rows);
        } else if (result.type == "modify") {
            std::cout << "\033[32m[OK]\033[0m " << result.message << "\n";
        } else {
            std::cout << "\033[32m[OK]\033[0m " << result.message << "\n";
        }
        std::cout << "\n";
    }

    std::cout << "Goodbye!\n";
    return 0;
}
