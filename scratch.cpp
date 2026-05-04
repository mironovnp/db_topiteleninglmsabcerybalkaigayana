#include <iostream>
#include "engine/parser.hpp"

using namespace db;

int main() {
    try {
        Lexer lexer("select q q");
        auto tokens = lexer.tokenize();
        for (auto& t : tokens) std::cout << "Token: " << t.value << "\n";
        Parser parser(tokens);
        parser.parse();
        std::cout << "Parsed!\n";
    } catch (std::exception& e) {
        std::cout << "Error: " << e.what() << "\n";
    }
    return 0;
}
