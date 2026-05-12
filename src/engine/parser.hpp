#pragma once
#include <string>
#include <vector>
#include <memory>
#include "engine/storage/storage.hpp"

namespace db {

// ── Tokens ─────────────────────────────────────────────────────────────

enum class TokenType {
    KW_CREATE, KW_DROP, KW_DATABASE, KW_TABLE,
    KW_SELECT, KW_FROM, KW_WHERE,
    KW_INSERT, KW_INTO, KW_VALUES, KW_LOAD, KW_CSV, KW_APPEND,
    KW_UPDATE, KW_SET, KW_DELETE,
    KW_AND, KW_OR, KW_NOT, KW_USE,
    KW_INT, KW_FLOAT, KW_BOOL, KW_TEXT, KW_VARCHAR,
    KW_PRIMARY, KW_KEY,
    KW_ORDER, KW_BY, KW_GROUP, KW_HAVING, KW_ASC, KW_DESC, KW_AS,
    KW_COUNT, KW_SUM, KW_AVG, KW_MIN, KW_MAX,
    KW_ALTER, KW_ADD, KW_COLUMN,
    KW_JOIN, KW_INNER, KW_LEFT, KW_RIGHT, KW_FULL, KW_CROSS, KW_OUTER, KW_ON,
    KW_LIMIT, KW_OFFSET,
    KW_IN, KW_EXISTS, KW_NULL, KW_UNIQUE, KW_DEFAULT, KW_FOREIGN, KW_REFERENCES, KW_CASCADE,
    KW_INDEX, KW_IF, KW_DISTINCT, KW_IS, KW_LIKE, KW_BETWEEN, KW_AUTOINCREMENT, KW_SHOW,
    KW_USER, KW_ROLE, KW_GRANT, KW_REVOKE, KW_TO, KW_PASSWORD, KW_ALL, KW_PRIVILEGES,
    KW_BEGIN, KW_COMMIT, KW_ROLLBACK,
    IDENTIFIER, STRING_LITERAL, NUMBER_LITERAL, BOOL_LITERAL,
    OP_EQ, OP_NEQ, OP_LT, OP_GT, OP_LTE, OP_GTE,
    OP_PLUS, OP_MINUS, OP_DIV, // OP_STAR is handled by STAR
    LPAREN, RPAREN, COMMA, SEMICOLON, STAR, DOT,
    END_OF_INPUT
};

struct Token { TokenType type; std::string value; };

// ── Token types and AggrFunc ──────────────────────────────────────────

enum class AggrFunc { NONE, COUNT, SUM, AVG, MIN, MAX };

// ── AST Base Classes ───────────────────────────────────────────────────

class ASTNode {
public:
    virtual ~ASTNode() = default;
};

class Expression : public ASTNode {
public:
    virtual ~Expression() = default;
};

class IsNullExpression : public Expression {
public:
    std::unique_ptr<Expression> operand;
    bool is_not;
    IsNullExpression(std::unique_ptr<Expression> op, bool n) : operand(std::move(op)), is_not(n) {}
};

class LikeExpression : public Expression {
public:
    std::unique_ptr<Expression> left;
    std::string pattern;
    bool negated;
    LikeExpression(std::unique_ptr<Expression> l, std::string p, bool n) : left(std::move(l)), pattern(std::move(p)), negated(n) {}
};

class BetweenExpression : public Expression {
public:
    std::unique_ptr<Expression> val;
    std::unique_ptr<Expression> low;
    std::unique_ptr<Expression> high;
    bool negated;
    BetweenExpression(std::unique_ptr<Expression> v, std::unique_ptr<Expression> l, std::unique_ptr<Expression> h, bool n)
        : val(std::move(v)), low(std::move(l)), high(std::move(h)), negated(n) {}
};

class Statement : public ASTNode {
public:
    virtual ~Statement() = default;
};

// ── Expressions ────────────────────────────────────────────────────────

class BinaryExpression : public Expression {
public:
    TokenType op;
    std::unique_ptr<Expression> left;
    std::unique_ptr<Expression> right;

    BinaryExpression(TokenType o, std::unique_ptr<Expression> l, std::unique_ptr<Expression> r)
        : op(o), left(std::move(l)), right(std::move(r)) {}
};

class UnaryExpression : public Expression {
public:
    TokenType op;
    std::unique_ptr<Expression> operand;

    UnaryExpression(TokenType o, std::unique_ptr<Expression> opnd)
        : op(o), operand(std::move(opnd)) {}
};

class LiteralExpression : public Expression {
public:
    std::string value;
    TokenType type;

    LiteralExpression(std::string v, TokenType t) : value(std::move(v)), type(t) {}
};

class ColumnExpression : public Expression {
public:
    std::string table;
    std::string column;

    ColumnExpression(std::string t, std::string c) : table(std::move(t)), column(std::move(c)) {}
};

class AggregateExpression : public Expression {
public:
    AggrFunc func;
    std::string column;
    bool distinct = false;

    AggregateExpression(AggrFunc f, std::string c, bool d = false) : func(f), column(std::move(c)), distinct(d) {}
};

class SelectStatement; // Forward declaration

class SubqueryExpression : public Expression {
public:
    std::unique_ptr<SelectStatement> subquery;
    bool is_exists;
    bool negated;

    SubqueryExpression(std::unique_ptr<SelectStatement> sq, bool exists, bool neg)
        : subquery(std::move(sq)), is_exists(exists), negated(neg) {}
};

class InListExpression : public Expression {
public:
    std::unique_ptr<Expression> left;
    std::vector<std::string> values;
    bool negated;

    InListExpression(std::unique_ptr<Expression> l, std::vector<std::string> vals, bool neg)
        : left(std::move(l)), values(std::move(vals)), negated(neg) {}
};

// ── Common Structures for Statements ───────────────────────────────────

enum class AlterAction { ADD_COL, DROP_COL };

struct ColDef {
    std::string name, type;
    bool is_primary_key = false;
    bool not_null = false;
    bool unique = false;
    bool has_default = false;
    bool is_autoincrement = false;
    std::string default_value;
    std::string fk_ref_table;
    std::string fk_ref_column;
    OnDeleteAction on_delete = OnDeleteAction::NO_ACTION;
    OnUpdateAction on_update = OnUpdateAction::NO_ACTION;
};

struct SetClause { std::string column; std::unique_ptr<Expression> value; };

struct SelectColumn {
    std::unique_ptr<Expression> expr;
    std::string alias; // optional alias for AS
};

struct OrderByClause {
    std::unique_ptr<Expression> expr;
    bool asc = true;
};

struct QualifiedCol {
    std::string table;
    std::string column;
};

struct JoinClause {
    enum Type { INNER, LEFT, RIGHT, FULL, CROSS };
    Type join_type = INNER;
    std::string table_name;
    std::string alias; // New: table alias
    QualifiedCol left_col;
    QualifiedCol right_col;
};

// ── Statements ─────────────────────────────────────────────────────────

class CreateDatabaseStatement : public Statement {
public:
    std::string database_name;
};

class DropDatabaseStatement : public Statement {
public:
    std::string database_name;
    bool if_exists = false;
};

class CreateTableStatement : public Statement {
public:
    std::string table_name;
    std::vector<ColDef> column_defs;
    int primary_key_index = -1;
};

class DropTableStatement : public Statement {
public:
    std::string table_name;
    bool if_exists = false;
};

class UseDatabaseStatement : public Statement {
public:
    std::string database_name;
};

class AlterTableStatement : public Statement {
public:
    std::string table_name;
    AlterAction alter_action = AlterAction::ADD_COL;
    std::string alter_col_name;
    std::string alter_col_type;
    ColDef alter_col_def;
    /// If set: after ADD COLUMN, fill the new column from CSV (PK column + new column in header).
    std::string add_column_csv_path;
};

class CreateIndexStatement : public Statement {
public:
    std::string index_name;
    std::string table_name;
    std::string column_name;
};

class DropIndexStatement : public Statement {
public:
    std::string index_name;
    std::string table_name;
};

class SelectStatement : public Statement {
public:
    bool distinct = false;
    bool select_all = false;
    std::vector<SelectColumn> select_columns;
    std::string table_name;
    std::string alias;
    std::unique_ptr<SelectStatement> from_subquery;
    std::string from_alias;
    std::vector<JoinClause> joins;
    std::unique_ptr<Expression> where;
    std::vector<std::string> group_by;
    std::unique_ptr<Expression> having;
    std::vector<OrderByClause> order_by;
    int limit = -1;
    int offset = 0;
};

class InsertStatement : public Statement {
public:
    std::string table_name;
    std::vector<std::string> insert_columns;
    std::vector<std::vector<std::unique_ptr<Expression>>> insert_values;
};

class UpdateStatement : public Statement {
public:
    std::string table_name;
    std::vector<SetClause> set_clauses;
    std::unique_ptr<Expression> where;
};

class DeleteStatement : public Statement {
public:
    std::string table_name;
    std::unique_ptr<Expression> where;
};

/// LOAD CSV 'path' INTO table_name [ ( col1, col2, ... ) ] [ APPEND ]
/// Without APPEND: table must be empty. With APPEND: rows are appended; header must match schema rules.
class LoadCsvStatement : public Statement {
public:
    std::string file_path;
    std::string table_name;
    /// If empty, CSV header must list all table columns.
    std::vector<std::string> columns;
    bool append = false;
};

class ShowStatement : public Statement {
public:
    enum Type { DATABASES, TABLES, COLUMNS, INDEX, CREATE_TABLE };
    Type type;
    std::string table_name; // used for COLUMNS, INDEX, CREATE_TABLE
};

class CreateUserStatement : public Statement {
public:
    std::string username;
    std::string password;
};

class SetUserStatement : public Statement {
public:
    std::string username;
    std::string password;
};

class CreateRoleStatement : public Statement {
public:
    std::string rolename;
};

class GrantRoleStatement : public Statement {
public:
    std::string role_name;
    std::string user_name;
};

class GrantStatement : public Statement {
public:
    std::string privilege;   // "SELECT", "INSERT", etc.
    std::string object_name; // Name of a table or all
    std::string role_name;
};

class BeginStatement : public Statement {};
class CommitStatement : public Statement {};
class RollbackStatement : public Statement {};

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
    std::unique_ptr<Statement> parse();
private:
    std::vector<Token> tokens_;
    size_t pos_ = 0;
    const Token& cur() const;
    Token consume();
    Token expect(TokenType t);
    bool match(TokenType t);
    bool check(TokenType t) const;

    std::unique_ptr<CreateDatabaseStatement> parseCreateDB();
    std::unique_ptr<DropDatabaseStatement> parseDropDB();
    std::unique_ptr<CreateTableStatement> parseCreateTable();
    std::unique_ptr<DropTableStatement> parseDropTable();
    std::unique_ptr<SelectStatement> parseSelect();
    std::unique_ptr<InsertStatement> parseInsert();
    std::unique_ptr<UpdateStatement> parseUpdate();
    std::unique_ptr<DeleteStatement> parseDelete();
    std::unique_ptr<UseDatabaseStatement> parseUse();
    std::unique_ptr<AlterTableStatement> parseAlterTable();
    std::unique_ptr<CreateIndexStatement> parseCreateIndex();
    std::unique_ptr<DropIndexStatement> parseDropIndex();
    std::unique_ptr<ShowStatement> parseShow();
    std::unique_ptr<LoadCsvStatement> parseLoadCsv();
    std::unique_ptr<CreateUserStatement> parseCreateUser();
    std::unique_ptr<SetUserStatement> parseSetUser();
    std::unique_ptr<CreateRoleStatement> parseCreateRole();
    std::unique_ptr<Statement> parseGrant();

    QualifiedCol parseQualifiedCol();
    std::string parseIdentifier();
    bool isIdentifier(TokenType t) const;

    std::unique_ptr<Expression> parseExprOr();
    std::unique_ptr<Expression> parseExprAnd();
    std::unique_ptr<Expression> parseExprNot();
    std::unique_ptr<Expression> parseExprCmp();
    std::unique_ptr<Expression> parseExprAddSub();
    std::unique_ptr<Expression> parseExprMulDiv();
    std::unique_ptr<Expression> parseExprAtom();
};

} // namespace db
