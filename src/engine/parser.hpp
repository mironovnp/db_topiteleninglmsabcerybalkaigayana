#pragma once
#include <string>
#include <vector>
#include <memory>

namespace db {

// ── Tokens ─────────────────────────────────────────────────────────────

enum class TokenType {
    KW_CREATE, KW_DROP, KW_DATABASE, KW_TABLE,
    KW_SELECT, KW_FROM, KW_WHERE,
    KW_INSERT, KW_INTO, KW_VALUES,
    KW_UPDATE, KW_SET, KW_DELETE,
    KW_AND, KW_OR, KW_NOT, KW_USE,
    KW_INT, KW_FLOAT, KW_BOOL, KW_TEXT, KW_VARCHAR,
    IDENTIFIER, STRING_LITERAL, NUMBER_LITERAL, BOOL_LITERAL,
    OP_EQ, OP_NEQ, OP_LT, OP_GT, OP_LTE, OP_GTE,
    LPAREN, RPAREN, COMMA, SEMICOLON, STAR,
    END_OF_INPUT
};

struct Token { TokenType type; std::string value; };

// ── WHERE expression tree ──────────────────────────────────────────────

struct WhereExpr {
    enum Kind { CMP, AND_OP, OR_OP, NOT_OP };
    Kind kind;
    std::string column, op, value;                   // CMP
    std::shared_ptr<WhereExpr> left, right;          // AND/OR/NOT(left only)
};

// ── Query representation ───────────────────────────────────────────────

enum class QueryType {
    CREATE_DATABASE, DROP_DATABASE,
    CREATE_TABLE, DROP_TABLE,
    SELECT, INSERT, UPDATE, DELETE_Q,
    USE_DATABASE
};

struct ColDef { std::string name, type; };
struct SetClause { std::string column, value; };

struct ParsedQuery {
    QueryType type;
    std::string database_name;
    std::string table_name;
    std::vector<ColDef> column_defs;
    std::vector<std::string> select_columns;
    bool select_all = false;
    std::vector<std::string> insert_columns;
    std::vector<std::vector<std::string>> insert_values;
    std::vector<SetClause> set_clauses;
    std::shared_ptr<WhereExpr> where;
};

// ── Lexer ──────────────────────────────────────────────────────────────

class Lexer {
public:
    explicit Lexer(const std::string& input);
    std::vector<Token> tokenize();
private:
    std::string input_;
    size_t pos_ = 0;
    void skipWS();
    Token readString();
    Token readNumber();
    Token readWord();
    Token readOp();
};

// ── Parser ─────────────────────────────────────────────────────────────

class Parser {
public:
    explicit Parser(const std::vector<Token>& tokens);
    ParsedQuery parse();
private:
    std::vector<Token> tokens_;
    size_t pos_ = 0;
    const Token& cur() const;
    Token consume();
    Token expect(TokenType t);
    bool match(TokenType t);
    bool check(TokenType t) const;

    ParsedQuery parseCreateDB();
    ParsedQuery parseDropDB();
    ParsedQuery parseCreateTable();
    ParsedQuery parseDropTable();
    ParsedQuery parseSelect();
    ParsedQuery parseInsert();
    ParsedQuery parseUpdate();
    ParsedQuery parseDelete();
    ParsedQuery parseUse();

    std::shared_ptr<WhereExpr> parseExprOr();
    std::shared_ptr<WhereExpr> parseExprAnd();
    std::shared_ptr<WhereExpr> parseExprNot();
    std::shared_ptr<WhereExpr> parseExprAtom();
};

} // namespace db
