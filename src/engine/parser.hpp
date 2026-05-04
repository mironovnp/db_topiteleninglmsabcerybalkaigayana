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
    KW_PRIMARY, KW_KEY,
    KW_ORDER, KW_BY, KW_GROUP, KW_HAVING, KW_ASC, KW_DESC, KW_AS,
    KW_COUNT, KW_SUM, KW_AVG, KW_MIN, KW_MAX,
    KW_ALTER, KW_ADD, KW_COLUMN,
    KW_JOIN, KW_INNER, KW_LEFT, KW_RIGHT, KW_OUTER, KW_ON,
    IDENTIFIER, STRING_LITERAL, NUMBER_LITERAL, BOOL_LITERAL,
    OP_EQ, OP_NEQ, OP_LT, OP_GT, OP_LTE, OP_GTE,
    LPAREN, RPAREN, COMMA, SEMICOLON, STAR, DOT,
    END_OF_INPUT
};

struct Token { TokenType type; std::string value; };

// ── Token types and AggrFunc ──────────────────────────────────────────

enum class AggrFunc { NONE, COUNT, SUM, AVG, MIN, MAX };

// ── WHERE expression tree ──────────────────────────────────────────────

struct WhereExpr {
    enum Kind { CMP, AND_OP, OR_OP, NOT_OP };
    Kind kind;
    std::string column, op, value;                   // CMP
    AggrFunc aggr = AggrFunc::NONE;                  // For HAVING
    std::shared_ptr<WhereExpr> left, right;          // AND/OR/NOT(left only)
};

// ── Query representation ───────────────────────────────────────────────

enum class QueryType {
    CREATE_DATABASE, DROP_DATABASE,
    CREATE_TABLE, DROP_TABLE,
    SELECT, INSERT, UPDATE, DELETE_Q,
    USE_DATABASE,
    ALTER_TABLE
};

struct ColDef { std::string name, type; bool is_primary_key = false; };
struct SetClause { std::string column, value; };

struct SelectColumn {
    std::string name; // column name or "*"
    AggrFunc aggr = AggrFunc::NONE;
    std::string alias; // optional alias for AS
};

struct OrderByClause {
    std::string column;
    AggrFunc aggr = AggrFunc::NONE;
    bool asc = true;
};

// JOIN clause: table alias for qualified column names (tbl.col)
struct QualifiedCol {
    std::string table;  // empty = unqualified
    std::string column;
};

struct JoinClause {
    enum Type { INNER, LEFT, RIGHT };
    Type join_type = INNER;
    std::string table_name;
    QualifiedCol left_col;   // ON left_col = right_col
    QualifiedCol right_col;
};

struct ParsedQuery {
    QueryType type;
    std::string database_name;
    std::string table_name;
    std::vector<ColDef> column_defs;
    int primary_key_index = -1;     // -1 = not specified (defaults to 0)
    std::vector<SelectColumn> select_columns;
    bool select_all = false;
    std::vector<std::string> insert_columns;
    std::vector<std::vector<std::string>> insert_values;
    std::vector<SetClause> set_clauses;
    std::shared_ptr<WhereExpr> where;
    std::vector<std::string> group_by;
    std::shared_ptr<WhereExpr> having;
    std::vector<OrderByClause> order_by;
    // JOIN
    std::vector<JoinClause> joins;
    // ALTER TABLE ADD COLUMN
    std::string alter_col_name;
    std::string alter_col_type;
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
    ParsedQuery parseAlterTable();

    QualifiedCol parseQualifiedCol();

    std::shared_ptr<WhereExpr> parseExprOr();
    std::shared_ptr<WhereExpr> parseExprAnd();
    std::shared_ptr<WhereExpr> parseExprNot();
    std::shared_ptr<WhereExpr> parseExprAtom();
};

} // namespace db
