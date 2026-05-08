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
    {"CASCADE",TokenType::KW_CASCADE},
    {"INDEX",TokenType::KW_INDEX},
    {"IF",TokenType::KW_IF},
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
        throw std::runtime_error("Expected DATABASE, TABLE or INDEX after CREATE, got: " + cur().value);
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
    if (check(KW_UPDATE)) { consume(); return parseUpdate(); }
    if (check(KW_DELETE)) { consume(); return parseDelete(); }
    if (check(KW_USE))    { consume(); return parseUse(); }
    throw std::runtime_error("Unknown query, got: " + cur().value);
}

// ── DDL ────────────────────────────────────────────────────────────────

std::unique_ptr<CreateDatabaseStatement> Parser::parseCreateDB() {
    expect(KW_DATABASE);
    auto q = std::make_unique<CreateDatabaseStatement>();
    q->database_name = expect(IDENTIFIER).value;
    match(SEMICOLON);
    return q;
}

std::unique_ptr<DropDatabaseStatement> Parser::parseDropDB() {
    expect(KW_DATABASE);
    auto q = std::make_unique<DropDatabaseStatement>();
    if (match(KW_IF)) { expect(KW_EXISTS); q->if_exists = true; }
    q->database_name = expect(IDENTIFIER).value;
    match(SEMICOLON);
    return q;
}

std::unique_ptr<CreateTableStatement> Parser::parseCreateTable() {
    expect(KW_TABLE);
    auto q = std::make_unique<CreateTableStatement>();
    q->table_name = expect(IDENTIFIER).value;
    expect(LPAREN);

    int col_idx = 0;
    do {
        ColDef col;
        col.name = expect(IDENTIFIER).value;

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
                col.fk_ref_table = expect(IDENTIFIER).value;
                expect(LPAREN);
                col.fk_ref_column = expect(IDENTIFIER).value;
                expect(RPAREN);
                if (match(KW_ON)) {
                    expect(KW_DELETE);
                    if (match(KW_CASCADE)) col.on_delete = OnDeleteAction::CASCADE;
                    else if (match(KW_SET)) {
                        expect(KW_NULL);
                        col.on_delete = OnDeleteAction::SET_NULL;
                    } else {
                        throw std::runtime_error("Expected CASCADE or SET NULL after ON DELETE");
                    }
                }
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
    q->table_name = expect(IDENTIFIER).value;
    match(SEMICOLON);
    return q;
}

std::unique_ptr<UseDatabaseStatement> Parser::parseUse() {
    auto q = std::make_unique<UseDatabaseStatement>();
    if (check(KW_DATABASE)) consume();
    q->database_name = expect(IDENTIFIER).value;
    match(SEMICOLON);
    return q;
}

std::unique_ptr<AlterTableStatement> Parser::parseAlterTable() {
    expect(KW_TABLE);
    auto q = std::make_unique<AlterTableStatement>();
    q->table_name = expect(IDENTIFIER).value;

    if (check(KW_ADD)) {
        consume();
        match(KW_COLUMN);
        q->alter_action = AlterAction::ADD_COL;
        q->alter_col_name = expect(IDENTIFIER).value;
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
                cd.fk_ref_table = expect(IDENTIFIER).value;
                expect(LPAREN); cd.fk_ref_column = expect(IDENTIFIER).value; expect(RPAREN);
            }
            else break;
        }
        q->alter_col_def = cd;
    } else if (check(KW_DROP)) {
        consume();
        match(KW_COLUMN);
        q->alter_action = AlterAction::DROP_COL;
        q->alter_col_name = expect(IDENTIFIER).value;
    } else {
        throw std::runtime_error("Expected ADD or DROP after ALTER TABLE, got: " + cur().value);
    }

    match(SEMICOLON);
    return q;
}

// ── Helper: parse [table.]column ────────────────────────────────────────

QualifiedCol Parser::parseQualifiedCol() {
    QualifiedCol qc;
    qc.column = expect(IDENTIFIER).value;
    if (check(DOT)) {
        consume();
        qc.table = qc.column;
        qc.column = expect(IDENTIFIER).value;
    }
    return qc;
}

// ── SELECT ─────────────────────────────────────────────────────────────

std::unique_ptr<SelectStatement> Parser::parseSelect() {
    auto q = std::make_unique<SelectStatement>();
    if (match(STAR)) {
        q->select_all = true;
    } else {
        do {
            SelectColumn sc;
            sc.expr = parseExprOr();
            if (match(KW_AS)) sc.alias = expect(IDENTIFIER).value;
            q->select_columns.push_back(std::move(sc));
        } while (match(COMMA));
    }
    expect(KW_FROM);
    q->table_name = expect(IDENTIFIER).value;
    if (match(KW_AS)) q->alias = expect(IDENTIFIER).value;
    else if (check(IDENTIFIER) && !check(KW_INNER) && !check(KW_LEFT) && !check(KW_RIGHT) && !check(KW_FULL) && !check(KW_CROSS) && !check(KW_JOIN) && !check(KW_WHERE) && !check(KW_GROUP) && !check(KW_ORDER) && !check(KW_LIMIT)) {
        q->alias = consume().value;
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
        jc.table_name = expect(IDENTIFIER).value;
        if (match(KW_AS)) jc.alias = expect(IDENTIFIER).value;
        else if (check(IDENTIFIER) && !check(KW_ON) && !check(KW_INNER) && !check(KW_LEFT) && !check(KW_RIGHT) && !check(KW_FULL) && !check(KW_CROSS) && !check(KW_JOIN) && !check(KW_WHERE)) {
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
            q->group_by.push_back(expect(IDENTIFIER).value);
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
    q->table_name = expect(IDENTIFIER).value;
    if (check(LPAREN)) {
        consume();
        do {
            q->insert_columns.push_back(expect(IDENTIFIER).value);
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
    q->table_name = expect(IDENTIFIER).value;
    expect(KW_SET);
    do {
        SetClause sc;
        sc.column = expect(IDENTIFIER).value;
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
    q->table_name = expect(IDENTIFIER).value;
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

    if (match(KW_COUNT)) { expect(LPAREN); std::string col = match(STAR) ? "*" : expect(IDENTIFIER).value; expect(RPAREN); return std::make_unique<AggregateExpression>(AggrFunc::COUNT, col); }
    if (match(KW_SUM)) { expect(LPAREN); std::string col = expect(IDENTIFIER).value; expect(RPAREN); return std::make_unique<AggregateExpression>(AggrFunc::SUM, col); }
    if (match(KW_AVG)) { expect(LPAREN); std::string col = expect(IDENTIFIER).value; expect(RPAREN); return std::make_unique<AggregateExpression>(AggrFunc::AVG, col); }
    if (match(KW_MIN)) { expect(LPAREN); std::string col = expect(IDENTIFIER).value; expect(RPAREN); return std::make_unique<AggregateExpression>(AggrFunc::MIN, col); }
    if (match(KW_MAX)) { expect(LPAREN); std::string col = expect(IDENTIFIER).value; expect(RPAREN); return std::make_unique<AggregateExpression>(AggrFunc::MAX, col); }

    Token t = cur();
    if (t.type == STRING_LITERAL || t.type == NUMBER_LITERAL || t.type == BOOL_LITERAL) {
        consume();
        return std::make_unique<LiteralExpression>(t.value, t.type);
    }
    
    if (t.type == KW_NULL) {
        consume();
        return std::make_unique<LiteralExpression>("NULL", KW_NULL);
    }

    if (t.type == IDENTIFIER) {
        std::string col = consume().value;
        std::string tbl = "";
        if (check(DOT)) {
            consume();
            tbl = col;
            col = expect(IDENTIFIER).value;
        }
        return std::make_unique<ColumnExpression>(tbl, col);
    }

    throw std::runtime_error("Unexpected token in expression: " + t.value);
}

// ── INDEXES ────────────────────────────────────────────────────────

std::unique_ptr<CreateIndexStatement> Parser::parseCreateIndex() {
    expect(KW_INDEX);
    auto q = std::make_unique<CreateIndexStatement>();
    q->index_name = expect(IDENTIFIER).value;
    expect(KW_ON);
    q->table_name = expect(IDENTIFIER).value;
    expect(LPAREN);
    q->column_name = expect(IDENTIFIER).value;
    expect(RPAREN);
    match(SEMICOLON);
    return q;
}

std::unique_ptr<DropIndexStatement> Parser::parseDropIndex() {
    expect(KW_INDEX);
    auto q = std::make_unique<DropIndexStatement>();
    q->index_name = expect(IDENTIFIER).value;
    expect(KW_ON);
    q->table_name = expect(IDENTIFIER).value;
    match(SEMICOLON);
    return q;
}

} // namespace db
