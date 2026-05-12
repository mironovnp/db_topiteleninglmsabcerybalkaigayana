#include "engine/parser.hpp"
#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <unordered_map>

namespace db {

// ════════════════════════════════════════════════════════════════════════
//  Lexer
// ════════════════════════════════════════════════════════════════════════

static const std::unordered_map<std::string, TokenType> KEYWORDS = {
    {"CREATE",TokenType::KW_CREATE},{"DROP",TokenType::KW_DROP},{"DATABASE",TokenType::KW_DATABASE},{"TABLE",TokenType::KW_TABLE},
    {"SELECT",TokenType::KW_SELECT},{"FROM",TokenType::KW_FROM},{"WHERE",TokenType::KW_WHERE},
    {"INSERT",TokenType::KW_INSERT},{"INTO",TokenType::KW_INTO},{"VALUES",TokenType::KW_VALUES},
    {"LOAD",TokenType::KW_LOAD},{"CSV",TokenType::KW_CSV},{"APPEND",TokenType::KW_APPEND},
    {"UPDATE",TokenType::KW_UPDATE},{"SET",TokenType::KW_SET},{"DELETE",TokenType::KW_DELETE},
    {"AND",TokenType::KW_AND},{"OR",TokenType::KW_OR},{"NOT",TokenType::KW_NOT},{"USE",TokenType::KW_USE},
    {"INT",TokenType::KW_INT},{"FLOAT",TokenType::KW_FLOAT},{"BOOL",TokenType::KW_BOOL},
    {"TEXT",TokenType::KW_TEXT},{"VARCHAR",TokenType::KW_VARCHAR},
    {"PRIMARY",TokenType::KW_PRIMARY},{"KEY",TokenType::KW_KEY},
    {"ORDER",TokenType::KW_ORDER},{"BY",TokenType::KW_BY},{"GROUP",TokenType::KW_GROUP},
    {"HAVING",TokenType::KW_HAVING},{"ASC",TokenType::KW_ASC},{"DESC",TokenType::KW_DESC},
    {"AS",TokenType::KW_AS},{"COUNT",TokenType::KW_COUNT},{"SUM",TokenType::KW_SUM},
    {"AVG",TokenType::KW_AVG},{"MIN",TokenType::KW_MIN},{"MAX",TokenType::KW_MAX},
    {"TRUE",TokenType::BOOL_LITERAL},{"FALSE",TokenType::BOOL_LITERAL},
    {"ALTER",TokenType::KW_ALTER},{"ADD",TokenType::KW_ADD},{"COLUMN",TokenType::KW_COLUMN},
    {"JOIN",TokenType::KW_JOIN},{"INNER",TokenType::KW_INNER},
    {"LEFT",TokenType::KW_LEFT},{"RIGHT",TokenType::KW_RIGHT},{"OUTER",TokenType::KW_OUTER},
    {"FULL",TokenType::KW_FULL},{"CROSS",TokenType::KW_CROSS},
    {"ON",TokenType::KW_ON},
    {"LIMIT",TokenType::KW_LIMIT},{"OFFSET",TokenType::KW_OFFSET},
    {"IN",TokenType::KW_IN},{"EXISTS",TokenType::KW_EXISTS},{"NULL",TokenType::KW_NULL},
    {"UNIQUE",TokenType::KW_UNIQUE},{"DEFAULT",TokenType::KW_DEFAULT},
    {"CASCADE",TokenType::KW_CASCADE},{"INDEX",TokenType::KW_INDEX},{"IF",TokenType::KW_IF},
    {"DISTINCT",TokenType::KW_DISTINCT},{"IS",TokenType::KW_IS},
    {"FOREIGN",TokenType::KW_FOREIGN},{"REFERENCES",TokenType::KW_REFERENCES},
    {"LIKE",TokenType::KW_LIKE},{"BETWEEN",TokenType::KW_BETWEEN},{"AUTOINCREMENT",TokenType::KW_AUTOINCREMENT},
    {"SHOW",TokenType::KW_SHOW}, 
    {"USER",TokenType::KW_USER}, {"ROLE",TokenType::KW_ROLE},
    {"GRANT",TokenType::KW_GRANT}, {"REVOKE",TokenType::KW_REVOKE},
    {"TO",TokenType::KW_TO}, {"PASSWORD",TokenType::KW_PASSWORD},
    {"ALL",TokenType::KW_ALL}, {"PRIVILEGES",TokenType::KW_PRIVILEGES},
    {"BEGIN",TokenType::KW_BEGIN}, {"COMMIT",TokenType::KW_COMMIT},
    {"ROLLBACK",TokenType::KW_ROLLBACK},
    {"REGISTER",TokenType::KW_REGISTER}, {"LOGIN",TokenType::KW_LOGIN},
    {"DDL",TokenType::KW_DDL},
};

using enum TokenType;

Lexer::Lexer(const std::string& input) : input_(input) {}

void Lexer::skipWS() {
    while (pos_ < input_.size() && std::isspace(static_cast<unsigned char>(input_[pos_])))
        ++pos_;
}

Token Lexer::readString() {
    char quote = input_[pos_++];
    std::string val;
    while (pos_ < input_.size() && input_[pos_] != quote) {
        if (input_[pos_] == '\\' && pos_ + 1 < input_.size()) { ++pos_; }
        val += input_[pos_++];
    }
    if (pos_ < input_.size()) ++pos_; // skip closing quote
    return {STRING_LITERAL, val};
}

Token Lexer::readNumber() {
    std::string val;
    bool dot = false;
    if (pos_ < input_.size() && input_[pos_] == '-') val += input_[pos_++];
    while (pos_ < input_.size() &&
           (std::isdigit(static_cast<unsigned char>(input_[pos_])) || input_[pos_] == '.')) {
        if (input_[pos_] == '.') { if (dot) break; dot = true; }
        val += input_[pos_++];
    }
    return {NUMBER_LITERAL, val};
}

Token Lexer::readWord() {
    std::string val;
    while (pos_ < input_.size() &&
           (std::isalnum(static_cast<unsigned char>(input_[pos_])) || input_[pos_] == '_'))
        val += input_[pos_++];

    std::string upper = val;
    std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);

    auto it = KEYWORDS.find(upper);
    if (it != KEYWORDS.end()) return {it->second, upper};
    return {IDENTIFIER, val};
}

Token Lexer::readOp() {
    char c = input_[pos_++];
    if (c == '=') return {OP_EQ, "="};
    if (c == '<') {
        if (pos_ < input_.size() && input_[pos_] == '=') { ++pos_; return {OP_LTE, "<="}; }
        return {OP_LT, "<"};
    }
    if (c == '>') {
        if (pos_ < input_.size() && input_[pos_] == '=') { ++pos_; return {OP_GTE, ">="}; }
        return {OP_GT, ">"};
    }
    if (c == '!' && pos_ < input_.size() && input_[pos_] == '=') { ++pos_; return {OP_NEQ, "!="}; }
    if (c == '(') return {LPAREN, "("};
    if (c == ')') return {RPAREN, ")"};
    if (c == ',') return {COMMA, ","};
    if (c == ';') return {SEMICOLON, ";"};
    if (c == '*') return {STAR, "*"};
    if (c == '.') return {DOT, "."};
    if (c == '+') return {OP_PLUS, "+"};
    if (c == '-') return {OP_MINUS, "-"};
    if (c == '/') return {OP_DIV, "/"};
    throw std::runtime_error(std::string("Unexpected character: ") + c);
}

std::vector<Token> Lexer::tokenize() {
    std::vector<Token> tokens;
    while (true) {
        skipWS();
        if (pos_ >= input_.size()) break;
        char c = input_[pos_];

        if (c == '\'' || c == '"')              tokens.push_back(readString());
        else if (std::isdigit(static_cast<unsigned char>(c)) ||
                 (c == '-' && pos_+1 < input_.size() &&
                  std::isdigit(static_cast<unsigned char>(input_[pos_+1]))))
                                                 tokens.push_back(readNumber());
        else if (std::isalpha(static_cast<unsigned char>(c)) || c == '_')
                                                 tokens.push_back(readWord());
        else                                     tokens.push_back(readOp());
    }
    tokens.push_back({END_OF_INPUT, "<EOF>"});
    return tokens;
}

// ════════════════════════════════════════════════════════════════════════
//  Parser
// ════════════════════════════════════════════════════════════════════════

Parser::Parser(const std::vector<Token>& tokens) : tokens_(tokens) {}

const Token& Parser::cur() const {
    static const Token eof_tok = {TokenType::END_OF_INPUT, "<EOF>"};
    if (pos_ < tokens_.size()) return tokens_[pos_];
    return eof_tok;
}

Token Parser::consume() { return tokens_[pos_++]; }

Token Parser::expect(TokenType t) {
    if (cur().type != t)
        throw std::runtime_error("Expected token type, got: " + cur().value);
    return consume();
}

bool Parser::isIdentifier(TokenType t) const {
    return t == IDENTIFIER || t == KW_COUNT || t == KW_SUM || 
           t == KW_AVG || t == KW_MIN || t == KW_MAX || t == KW_INDEX ||
           t == KW_LOGIN || t == KW_REGISTER;
}

std::string Parser::parseIdentifier() {
    Token t = cur();
    if (isIdentifier(t.type)) {
        return consume().value;
    }
    throw std::runtime_error("Expected identifier, got: " + t.value);
}

bool Parser::match(TokenType t) {
    if (cur().type == t) { ++pos_; return true; }
    return false;
}

bool Parser::check(TokenType t) const { return cur().type == t; }

// ── Main dispatch ──────────────────────────────────────────────────────

std::unique_ptr<Statement> Parser::parse() {
    if (check(KW_CREATE)) {
        consume();
        if (check(KW_DATABASE)) return parseCreateDB();
        if (check(KW_TABLE))    return parseCreateTable();
        if (check(KW_INDEX))    return parseCreateIndex();
        if (check(KW_USER))     return parseCreateUser();
        if (check(KW_ROLE))     return parseCreateRole();
        throw std::runtime_error("Expected DATABASE, TABLE, INDEX, USER or ROLE after CREATE, got: " + cur().value);
    }
    if (check(KW_DROP)) {
        consume();
        if (check(KW_DATABASE)) return parseDropDB();
        if (check(KW_TABLE))    return parseDropTable();
        if (check(KW_INDEX))    return parseDropIndex();
        throw std::runtime_error("Expected DATABASE, TABLE or INDEX after DROP, got: " + cur().value);
    }
    if (check(KW_ALTER)) { consume(); return parseAlterTable(); }
    if (check(KW_SELECT)) { consume(); return parseSelect(); }
    if (check(KW_INSERT)) { consume(); return parseInsert(); }
    if (check(KW_LOAD)) {
        consume();
        expect(KW_CSV);
        return parseLoadCsv();
    }
    if (check(KW_UPDATE)) { consume(); return parseUpdate(); }
    if (check(KW_DELETE)) { consume(); return parseDelete(); }
    if (check(KW_USE))    { consume(); return parseUse(); }
    if (check(KW_SHOW))   { consume(); return parseShow(); }
    if (check(KW_GRANT))  { consume(); return parseGrant(); }
    if (check(KW_BEGIN)) { consume(); match(SEMICOLON); return std::make_unique<BeginStatement>(); }
    if (check(KW_COMMIT)) { consume(); match(SEMICOLON); return std::make_unique<CommitStatement>(); }
    if (check(KW_ROLLBACK)) { consume(); match(SEMICOLON); return std::make_unique<RollbackStatement>(); }
    if (check(KW_SET)) {
        consume(); 
        if (check(KW_USER)) return parseSetUser();
        throw std::runtime_error("Expected USER after SET");
    }
    if (check(KW_REGISTER)) { consume(); return parseRegister(); }
    if (check(KW_LOGIN)) { consume(); return parseLogin(); }
    throw std::runtime_error("Unknown query, got: " + cur().value);
}

// ── DDL ────────────────────────────────────────────────────────────────

std::unique_ptr<CreateDatabaseStatement> Parser::parseCreateDB() {
    expect(KW_DATABASE);
    auto q = std::make_unique<CreateDatabaseStatement>();
    q->database_name = parseIdentifier();
    match(SEMICOLON);
    return q;
}

std::unique_ptr<DropDatabaseStatement> Parser::parseDropDB() {
    expect(KW_DATABASE);
    auto q = std::make_unique<DropDatabaseStatement>();
    if (match(KW_IF)) { expect(KW_EXISTS); q->if_exists = true; }
    q->database_name = parseIdentifier();
    match(SEMICOLON);
    return q;
}

std::unique_ptr<CreateTableStatement> Parser::parseCreateTable() {
    expect(KW_TABLE);
    auto q = std::make_unique<CreateTableStatement>();
    q->table_name = parseIdentifier();
    expect(LPAREN);

    int col_idx = 0;
    do {
        if (match(KW_FOREIGN)) {
            expect(KW_KEY);
            expect(LPAREN);
            std::string col_name = parseIdentifier();
            expect(RPAREN);
            expect(KW_REFERENCES);
            std::string ref_table = parseIdentifier();
            expect(LPAREN);
            std::string ref_col = parseIdentifier();
            expect(RPAREN);

            // Find the column to attach this FK to
            bool found = false;
            for (auto& existing_col : q->column_defs) {
                if (existing_col.name == col_name) {
                    existing_col.fk_ref_table = ref_table;
                    existing_col.fk_ref_column = ref_col;
                    
                    // Parse ON DELETE / ON UPDATE
                    while (match(KW_ON)) {
                        if (match(KW_DELETE)) {
                            if (match(KW_CASCADE)) existing_col.on_delete = OnDeleteAction::CASCADE;
                            else if (match(KW_SET)) { expect(KW_NULL); existing_col.on_delete = OnDeleteAction::SET_NULL; }
                            else throw std::runtime_error("Expected CASCADE or SET NULL after ON DELETE");
                        } else if (match(KW_UPDATE)) {
                            if (match(KW_CASCADE)) existing_col.on_update = OnUpdateAction::CASCADE;
                            else if (match(KW_SET)) { expect(KW_NULL); existing_col.on_update = OnUpdateAction::SET_NULL; }
                            else throw std::runtime_error("Expected CASCADE or SET NULL after ON UPDATE");
                        } else {
                            throw std::runtime_error("Expected DELETE or UPDATE after ON");
                        }
                    }
                    found = true;
                    break;
                }
            }
            if (!found) throw std::runtime_error("FOREIGN KEY column '" + col_name + "' not found in table definition");
            continue;
        }

        ColDef col;
        col.name = parseIdentifier();

        Token tt = consume();
        std::string tp = tt.value;
        if (tp == "VARCHAR") {
            expect(LPAREN);
            tp += "(" + expect(NUMBER_LITERAL).value + ")";
            expect(RPAREN);
        }
        col.type = tp;

        while (true) {
            if (check(KW_PRIMARY)) {
                consume(); expect(KW_KEY);
                col.is_primary_key = true;
                q->primary_key_index = col_idx;
            } else if (check(KW_NOT)) {
                consume(); expect(KW_NULL);
                col.not_null = true;
            } else if (check(KW_UNIQUE)) {
                consume();
                col.unique = true;
            } else if (check(KW_DEFAULT)) {
                consume();
                col.has_default = true;
                col.default_value = consume().value;
            } else if (check(KW_REFERENCES)) {
                consume();
                col.fk_ref_table = parseIdentifier();
                expect(LPAREN);
                col.fk_ref_column = parseIdentifier();
                expect(RPAREN);
                while (match(KW_ON)) {
                    if (match(KW_DELETE)) {
                        if (match(KW_CASCADE)) col.on_delete = OnDeleteAction::CASCADE;
                        else if (match(KW_SET)) { expect(KW_NULL); col.on_delete = OnDeleteAction::SET_NULL; }
                        else throw std::runtime_error("Expected CASCADE or SET NULL after ON DELETE");
                    } else if (match(KW_UPDATE)) {
                        if (match(KW_CASCADE)) col.on_update = OnUpdateAction::CASCADE;
                        else if (match(KW_SET)) { expect(KW_NULL); col.on_update = OnUpdateAction::SET_NULL; }
                        else throw std::runtime_error("Expected CASCADE or SET NULL after ON UPDATE");
                    } else {
                        throw std::runtime_error("Expected DELETE or UPDATE after ON");
                    }
                }
            } else if (check(KW_AUTOINCREMENT)) {
                consume();
                col.is_autoincrement = true;
            } else {
                break;
            }
        }

        q->column_defs.push_back(col);
        ++col_idx;
    } while (match(COMMA));

    expect(RPAREN);
    match(SEMICOLON);
    return q;
}

std::unique_ptr<DropTableStatement> Parser::parseDropTable() {
    expect(KW_TABLE);
    auto q = std::make_unique<DropTableStatement>();
    if (match(KW_IF)) { expect(KW_EXISTS); q->if_exists = true; }
    q->table_name = parseIdentifier();
    match(SEMICOLON);
    return q;
}

std::unique_ptr<UseDatabaseStatement> Parser::parseUse() {
    auto q = std::make_unique<UseDatabaseStatement>();
    if (check(KW_DATABASE)) consume();
    q->database_name = parseIdentifier();
    match(SEMICOLON);
    return q;
}

std::unique_ptr<AlterTableStatement> Parser::parseAlterTable() {
    expect(KW_TABLE);
    auto q = std::make_unique<AlterTableStatement>();
    q->table_name = parseIdentifier();

    if (check(KW_ADD)) {
        consume();
        match(KW_COLUMN);
        q->alter_action = AlterAction::ADD_COL;
        q->alter_col_name = parseIdentifier();
        Token tt = consume();
        std::string tp = tt.value;
        if (tp == "VARCHAR") {
            expect(LPAREN);
            tp += "(" + expect(NUMBER_LITERAL).value + ")";
            expect(RPAREN);
        }
        q->alter_col_type = tp;
        ColDef cd;
        cd.name = q->alter_col_name;
        cd.type = tp;
        while (true) {
            if (check(KW_NOT)) { consume(); expect(KW_NULL); cd.not_null = true; }
            else if (check(KW_UNIQUE)) { consume(); cd.unique = true; }
            else if (check(KW_DEFAULT)) { consume(); cd.has_default = true; cd.default_value = consume().value; }
            else if (check(KW_REFERENCES)) {
                consume();
                cd.fk_ref_table = parseIdentifier();
                expect(LPAREN); cd.fk_ref_column = parseIdentifier(); expect(RPAREN);
            }
            else break;
        }
        q->alter_col_def = cd;
        if (match(KW_FROM)) {
            expect(KW_CSV);
            if (cur().type != STRING_LITERAL)
                throw std::runtime_error(
                    "ALTER TABLE ADD COLUMN FROM CSV: expected file path string literal");
            q->add_column_csv_path = consume().value;
        }
    } else if (check(KW_DROP)) {
        consume();
        match(KW_COLUMN);
        q->alter_action = AlterAction::DROP_COL;
        q->alter_col_name = parseIdentifier();
    } else {
        throw std::runtime_error("Expected ADD or DROP after ALTER TABLE, got: " + cur().value);
    }

    match(SEMICOLON);
    return q;
}

// ── DCL (Data Control Language) ────────────────────────────────────────

std::unique_ptr<CreateUserStatement> Parser::parseCreateUser() {
    expect(KW_USER);
    auto q = std::make_unique<CreateUserStatement>();
    q->username = expect(IDENTIFIER).value;
    
    if (match(KW_PASSWORD)) {
        q->password = expect(STRING_LITERAL).value;
    }
    match(SEMICOLON);
    return q;
}

std::unique_ptr<CreateRoleStatement> Parser::parseCreateRole() {
    expect(KW_ROLE);
    auto q = std::make_unique<CreateRoleStatement>();
    q->rolename = expect(IDENTIFIER).value;
    match(SEMICOLON);
    return q;
}

std::unique_ptr<Statement> Parser::parseGrant() {
    // Branch 1: GRANT ROLE <role> TO <user>;
    if (match(KW_ROLE)) {
        auto q = std::make_unique<GrantRoleStatement>();
        q->role_name = expect(IDENTIFIER).value;
        expect(KW_TO);
        q->user_name = expect(IDENTIFIER).value;
        match(SEMICOLON);
        return q;
    }
    // Branch 2: GRANT DDL ON <db> TO <user>;
    if (match(KW_DDL)) {
        auto q = std::make_unique<GrantDdlStatement>();
        expect(KW_ON);
        q->db_name = expect(IDENTIFIER).value;
        expect(KW_TO);
        q->username = expect(IDENTIFIER).value;
        match(SEMICOLON);
        return q;
    }
    // Branch 3: GRANT <privilege> ON <object> TO <role>;
    else {
        auto q = std::make_unique<GrantStatement>();
        
        if (match(KW_ALL)) {
            q->privilege = "ALL";
            match(KW_PRIVILEGES); // Ignoring optional PRIVILEGES
        } else {
            // Getting the word (SELECT, INSERT, UPDATE)
            q->privilege = consume().value; 
        }
        
        expect(KW_ON);
        
        if (match(STAR)) {
            q->object_name = "*";
        } else {
            q->object_name = expect(IDENTIFIER).value;
        }
        
        expect(KW_TO);
        q->role_name = expect(IDENTIFIER).value;
        match(SEMICOLON);
        return q;
    }
}

std::unique_ptr<RegisterStatement> Parser::parseRegister() {
    auto q = std::make_unique<RegisterStatement>();
    q->username = expect(IDENTIFIER).value;
    expect(KW_PASSWORD);
    q->password = expect(STRING_LITERAL).value;
    match(SEMICOLON);
    return q;
}

std::unique_ptr<LoginStatement> Parser::parseLogin() {
    auto q = std::make_unique<LoginStatement>();
    q->username = expect(IDENTIFIER).value;
    expect(KW_PASSWORD);
    q->password = expect(STRING_LITERAL).value;
    match(SEMICOLON);
    return q;
}

// ── Helper: parse [table.]column ────────────────────────────────────────

QualifiedCol Parser::parseQualifiedCol() {
    QualifiedCol qc;
    qc.column = parseIdentifier();
    if (check(DOT)) {
        consume();
        qc.table = qc.column;
        qc.column = parseIdentifier();
    }
    return qc;
}

// ── SELECT ─────────────────────────────────────────────────────────────

std::unique_ptr<SelectStatement> Parser::parseSelect() {
    auto q = std::make_unique<SelectStatement>();
    if (match(KW_DISTINCT)) q->distinct = true;

    if (match(STAR)) {
        q->select_all = true;
    } else {
        do {
            SelectColumn sc;
            sc.expr = parseExprOr();
            if (match(KW_AS)) sc.alias = parseIdentifier();
            q->select_columns.push_back(std::move(sc));
        } while (match(COMMA));
    }
    expect(KW_FROM);
    if (match(LPAREN)) {
        expect(KW_SELECT);
        q->from_subquery = parseSelect();
        expect(RPAREN);
        if (match(KW_AS)) q->from_alias = parseIdentifier();
        else q->from_alias = parseIdentifier();
    } else {
        q->table_name = parseIdentifier();
        if (match(KW_AS)) q->alias = parseIdentifier();
        else if (isIdentifier(cur().type) && !check(KW_INNER) && !check(KW_LEFT) && !check(KW_RIGHT) && !check(KW_FULL) && !check(KW_CROSS) && !check(KW_JOIN) && !check(KW_WHERE) && !check(KW_GROUP) && !check(KW_ORDER) && !check(KW_LIMIT)) {
            q->alias = consume().value;
        }
    }

    while (true) {
        bool is_cross = false;
        JoinClause::Type jtype = JoinClause::INNER;
        if (check(KW_INNER)) {
            consume();
            expect(KW_JOIN);
        } else if (check(KW_LEFT)) {
            consume();
            match(KW_OUTER);
            expect(KW_JOIN);
            jtype = JoinClause::LEFT;
        } else if (check(KW_RIGHT)) {
            consume();
            match(KW_OUTER);
            expect(KW_JOIN);
            jtype = JoinClause::RIGHT;
        } else if (check(KW_FULL)) {
            consume();
            match(KW_OUTER);
            expect(KW_JOIN);
            jtype = JoinClause::FULL;
        } else if (check(KW_CROSS)) {
            consume();
            expect(KW_JOIN);
            jtype = JoinClause::CROSS;
            is_cross = true;
        } else if (check(KW_JOIN)) {
            consume();
        } else {
            break;
        }
        JoinClause jc;
        jc.join_type = jtype;
        jc.table_name = parseIdentifier();
        if (match(KW_AS)) jc.alias = parseIdentifier();
        else if (isIdentifier(cur().type) && !check(KW_ON) && !check(KW_INNER) && !check(KW_LEFT) && !check(KW_RIGHT) && !check(KW_FULL) && !check(KW_CROSS) && !check(KW_JOIN) && !check(KW_WHERE)) {
            jc.alias = consume().value;
        }

        if (!is_cross) {
            expect(KW_ON);
            jc.left_col  = parseQualifiedCol();
            expect(OP_EQ);
            jc.right_col = parseQualifiedCol();
        }
        q->joins.push_back(std::move(jc));
    }

    if (match(KW_WHERE)) q->where = parseExprOr();
    if (match(KW_GROUP)) {
        expect(KW_BY);
        do {
            auto qc = parseQualifiedCol();
            std::string full_name = qc.table.empty() ? qc.column : qc.table + "." + qc.column;
            q->group_by.push_back(full_name);
        } while (match(COMMA));
    }
    if (match(KW_HAVING)) q->having = parseExprOr();
    if (match(KW_ORDER)) {
        expect(KW_BY);
        do {
            OrderByClause ob;
            ob.expr = parseExprOr();
            if (match(KW_DESC)) ob.asc = false;
            else match(KW_ASC);
            q->order_by.push_back(std::move(ob));
        } while (match(COMMA));
    }
    if (match(KW_LIMIT)) {
        q->limit = std::stoi(expect(NUMBER_LITERAL).value);
        if (match(KW_OFFSET))
            q->offset = std::stoi(expect(NUMBER_LITERAL).value);
    }
    match(SEMICOLON);
    return q;
}

// ── INSERT, UPDATE, DELETE ─────────────────────────────────────────────

std::unique_ptr<InsertStatement> Parser::parseInsert() {
    auto q = std::make_unique<InsertStatement>();
    expect(KW_INTO);
    q->table_name = parseIdentifier();
    if (check(LPAREN)) {
        consume();
        do {
            q->insert_columns.push_back(parseIdentifier());
        } while (match(COMMA));
        expect(RPAREN);
    }
    expect(KW_VALUES);
    do {
        expect(LPAREN);
        std::vector<std::unique_ptr<Expression>> vals;
        do {
            vals.push_back(parseExprOr());
        } while (match(COMMA));
        expect(RPAREN);
        q->insert_values.push_back(std::move(vals));
    } while (match(COMMA));
    match(SEMICOLON);
    return q;
}

std::unique_ptr<UpdateStatement> Parser::parseUpdate() {
    auto q = std::make_unique<UpdateStatement>();
    q->table_name = parseIdentifier();
    expect(KW_SET);
    do {
        SetClause sc;
        sc.column = parseIdentifier();
        expect(OP_EQ);
        sc.value = parseExprOr();
        q->set_clauses.push_back(std::move(sc));
    } while (match(COMMA));
    if (match(KW_WHERE)) q->where = parseExprOr();
    match(SEMICOLON);
    return q;
}

std::unique_ptr<DeleteStatement> Parser::parseDelete() {
    auto q = std::make_unique<DeleteStatement>();
    expect(KW_FROM);
    q->table_name = parseIdentifier();
    if (match(KW_WHERE)) q->where = parseExprOr();
    match(SEMICOLON);
    return q;
}

// ── EXPRESSIONS (Precedence Climbing / Recursive Descent) ──────────────

std::unique_ptr<Expression> Parser::parseExprOr() {
    auto left = parseExprAnd();
    while (match(KW_OR)) {
        auto right = parseExprAnd();
        left = std::make_unique<BinaryExpression>(KW_OR, std::move(left), std::move(right));
    }
    return left;
}

std::unique_ptr<Expression> Parser::parseExprAnd() {
    auto left = parseExprNot();
    while (match(KW_AND)) {
        auto right = parseExprNot();
        left = std::make_unique<BinaryExpression>(KW_AND, std::move(left), std::move(right));
    }
    return left;
}

std::unique_ptr<Expression> Parser::parseExprNot() {
    if (match(KW_NOT)) {
        return std::make_unique<UnaryExpression>(KW_NOT, parseExprNot());
    }
    return parseExprCmp();
}

std::unique_ptr<Expression> Parser::parseExprCmp() {
    auto left = parseExprAddSub();

    if (match(KW_IN)) {
        expect(LPAREN);
        if (check(KW_SELECT)) {
            consume();
            auto subq = std::make_unique<SubqueryExpression>(parseSelect(), false, false);
            expect(RPAREN);
            return std::make_unique<BinaryExpression>(KW_IN, std::move(left), std::move(subq));
        } else {
            std::vector<std::string> vals;
            do {
                vals.push_back(consume().value);
            } while (match(COMMA));
            expect(RPAREN);
            return std::make_unique<InListExpression>(std::move(left), std::move(vals), false);
        }
    }

    if (check(KW_NOT)) {
        size_t saved = pos_;
        consume();
        if (match(KW_IN)) {
            expect(LPAREN);
            if (check(KW_SELECT)) {
                consume();
                auto subq = std::make_unique<SubqueryExpression>(parseSelect(), false, true);
                expect(RPAREN);
                return std::make_unique<BinaryExpression>(KW_IN, std::move(left), std::move(subq)); // negation is inside subquery/inlist for now
            } else {
                std::vector<std::string> vals;
                do {
                    vals.push_back(consume().value);
                } while (match(COMMA));
                expect(RPAREN);
                return std::make_unique<InListExpression>(std::move(left), std::move(vals), true);
            }
        }
        pos_ = saved;
    }

    TokenType opTok = cur().type;
    if (opTok == OP_EQ || opTok == OP_NEQ || opTok == OP_LT || opTok == OP_GT || opTok == OP_LTE || opTok == OP_GTE) {
        consume();
        auto right = parseExprAddSub();
        left = std::make_unique<BinaryExpression>(opTok, std::move(left), std::move(right));
    }

    if (match(KW_IS)) {
        bool is_not = match(KW_NOT);
        expect(KW_NULL);
        return std::make_unique<IsNullExpression>(std::move(left), is_not);
    }

    if (match(KW_LIKE)) {
        std::string p = expect(STRING_LITERAL).value;
        return std::make_unique<LikeExpression>(std::move(left), p, false);
    }
    
    if (match(KW_BETWEEN)) {
        auto low = parseExprAddSub();
        expect(KW_AND);
        auto high = parseExprAddSub();
        return std::make_unique<BetweenExpression>(std::move(left), std::move(low), std::move(high), false);
    }

    if (check(KW_NOT)) {
        size_t saved = pos_;
        consume();
        if (match(KW_LIKE)) {
            std::string p = expect(STRING_LITERAL).value;
            return std::make_unique<LikeExpression>(std::move(left), p, true);
        }
        if (match(KW_BETWEEN)) {
            auto low = parseExprAddSub();
            expect(KW_AND);
            auto high = parseExprAddSub();
            return std::make_unique<BetweenExpression>(std::move(left), std::move(low), std::move(high), true);
        }
        pos_ = saved;
    }

    return left;
}

std::unique_ptr<Expression> Parser::parseExprAddSub() {
    auto left = parseExprMulDiv();
    while (check(OP_PLUS) || check(OP_MINUS)) {
        TokenType op = consume().type;
        auto right = parseExprMulDiv();
        left = std::make_unique<BinaryExpression>(op, std::move(left), std::move(right));
    }
    return left;
}

std::unique_ptr<Expression> Parser::parseExprMulDiv() {
    auto left = parseExprAtom();
    while (check(STAR) || check(OP_DIV)) {
        TokenType op = consume().type;
        auto right = parseExprAtom();
        left = std::make_unique<BinaryExpression>(op, std::move(left), std::move(right));
    }
    return left;
}

std::unique_ptr<Expression> Parser::parseExprAtom() {
    if (match(OP_MINUS)) {
        return std::make_unique<UnaryExpression>(OP_MINUS, parseExprAtom());
    }

    if (match(LPAREN)) {
        if (check(KW_SELECT)) {
            consume();
            auto subq = std::make_unique<SubqueryExpression>(parseSelect(), false, false);
            expect(RPAREN);
            return subq;
        }
        auto expr = parseExprOr();
        expect(RPAREN);
        return expr;
    }

    if (check(KW_EXISTS)) {
        consume();
        expect(LPAREN);
        expect(KW_SELECT);
        auto subq = std::make_unique<SubqueryExpression>(parseSelect(), true, false);
        expect(RPAREN);
        return subq;
    }

    if (check(KW_NOT) && pos_ + 1 < tokens_.size() && tokens_[pos_ + 1].type == KW_EXISTS) {
        consume(); // NOT
        consume(); // EXISTS
        expect(LPAREN);
        expect(KW_SELECT);
        auto subq = std::make_unique<SubqueryExpression>(parseSelect(), true, true);
        expect(RPAREN);
        return subq;
    }

    auto parse_aggr = [&](AggrFunc f) {
        expect(LPAREN);
        bool d = match(KW_DISTINCT);
        std::string col;
        if (f == AggrFunc::COUNT && match(STAR)) col = "*";
        else col = parseIdentifier();
        expect(RPAREN);
        return std::make_unique<AggregateExpression>(f, col, d);
    };

    auto is_aggr_call = [&](TokenType t) {
        return check(t) && pos_ + 1 < tokens_.size() && tokens_[pos_ + 1].type == LPAREN;
    };

    if (is_aggr_call(KW_COUNT)) { consume(); return parse_aggr(AggrFunc::COUNT); }
    if (is_aggr_call(KW_SUM)) { consume(); return parse_aggr(AggrFunc::SUM); }
    if (is_aggr_call(KW_AVG)) { consume(); return parse_aggr(AggrFunc::AVG); }
    if (is_aggr_call(KW_MIN)) { consume(); return parse_aggr(AggrFunc::MIN); }
    if (is_aggr_call(KW_MAX)) { consume(); return parse_aggr(AggrFunc::MAX); }

    Token t = cur();
    if (t.type == STRING_LITERAL || t.type == NUMBER_LITERAL || t.type == BOOL_LITERAL) {
        consume();
        return std::make_unique<LiteralExpression>(t.value, t.type);
    }
    
    if (t.type == KW_NULL) {
        consume();
        return std::make_unique<LiteralExpression>("NULL", KW_NULL);
    }

    if (isIdentifier(t.type)) {
        std::string col = consume().value;
        std::string tbl = "";
        if (check(DOT)) {
            consume();
            tbl = col;
            col = parseIdentifier();
        }
        return std::make_unique<ColumnExpression>(tbl, col);
    }

    throw std::runtime_error("Unexpected token in expression: " + t.value);
}

// ── INDEXES ────────────────────────────────────────────────────────

std::unique_ptr<CreateIndexStatement> Parser::parseCreateIndex() {
    expect(KW_INDEX);
    auto q = std::make_unique<CreateIndexStatement>();
    q->index_name = parseIdentifier();
    expect(KW_ON);
    q->table_name = parseIdentifier();
    expect(LPAREN);
    q->column_name = parseIdentifier();
    expect(RPAREN);
    match(SEMICOLON);
    return q;
}

std::unique_ptr<DropIndexStatement> Parser::parseDropIndex() {
    expect(KW_INDEX);
    auto q = std::make_unique<DropIndexStatement>();
    q->index_name = parseIdentifier();
    expect(KW_ON);
    q->table_name = parseIdentifier();
    match(SEMICOLON);
    return q;
}

std::unique_ptr<ShowStatement> Parser::parseShow() {
    auto q = std::make_unique<ShowStatement>();
    Token t = consume();
    std::string type = t.value;
    std::transform(type.begin(), type.end(), type.begin(), ::toupper);

    if (type == "DATABASES") {
        q->type = ShowStatement::DATABASES;
    } else if (type == "TABLES") {
        q->type = ShowStatement::TABLES;
    } else if (type == "COLUMNS") {
        q->type = ShowStatement::COLUMNS;
        if (match(KW_FROM) || match(KW_IN)) {
            q->table_name = parseIdentifier();
        } else {
            throw std::runtime_error("Expected FROM or IN after SHOW COLUMNS");
        }
    } else if (type == "INDEX" || t.type == KW_INDEX) {
        q->type = ShowStatement::INDEX;
        if (match(KW_FROM) || match(KW_IN)) {
            q->table_name = parseIdentifier();
        } else {
            throw std::runtime_error("Expected FROM or IN after SHOW INDEX");
        }
    } else if (type == "CREATE" || t.type == KW_CREATE) {
        expect(KW_TABLE);
        q->type = ShowStatement::CREATE_TABLE;
        q->table_name = parseIdentifier();
    } else {
        throw std::runtime_error("Unknown SHOW command: SHOW " + type);
    }

    match(SEMICOLON);
    return q;
}

std::unique_ptr<LoadCsvStatement> Parser::parseLoadCsv() {
    auto q = std::make_unique<LoadCsvStatement>();
    if (cur().type != STRING_LITERAL)
        throw std::runtime_error("LOAD CSV: expected file path string literal");
    q->file_path = consume().value;
    expect(KW_INTO);
    q->table_name = parseIdentifier();
    if (match(LPAREN)) {
        do {
            q->columns.push_back(parseIdentifier());
        } while (match(COMMA));
        expect(RPAREN);
    }
    if (match(KW_APPEND))
        q->append = true;
    match(SEMICOLON);
    return q;
}

std::unique_ptr<SetUserStatement> Parser::parseSetUser() {
    expect(KW_USER);
    auto q = std::make_unique<SetUserStatement>();
    
    if (check(TokenType::STRING_LITERAL)) {
        q->username = consume().value;
    } else {
        q->username = expect(TokenType::IDENTIFIER).value;
    }
    if (match(TokenType::KW_PASSWORD)){
        q->password = expect(TokenType::STRING_LITERAL).value;
    }
    match(TokenType::SEMICOLON);
    return q;
}

} // namespace db
