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
    {"ON",TokenType::KW_ON},
};

// Bring enum values into scope for readability
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

ParsedQuery Parser::parse() {
    if (check(KW_CREATE)) {
        consume();
        if (check(KW_DATABASE)) return parseCreateDB();
        if (check(KW_TABLE))    return parseCreateTable();
        throw std::runtime_error("Expected DATABASE or TABLE after CREATE, got: " + cur().value);
    }
    if (check(KW_DROP)) {
        consume();
        if (check(KW_DATABASE)) return parseDropDB();
        if (check(KW_TABLE))    return parseDropTable();
        throw std::runtime_error("Expected DATABASE or TABLE after DROP, got: " + cur().value);
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

ParsedQuery Parser::parseCreateDB() {
    expect(KW_DATABASE);
    ParsedQuery q; q.type = QueryType::CREATE_DATABASE;
    q.database_name = expect(IDENTIFIER).value;
    match(SEMICOLON);
    return q;
}

ParsedQuery Parser::parseDropDB() {
    expect(KW_DATABASE);
    ParsedQuery q; q.type = QueryType::DROP_DATABASE;
    q.database_name = expect(IDENTIFIER).value;
    match(SEMICOLON);
    return q;
}

ParsedQuery Parser::parseCreateTable() {
    expect(KW_TABLE);
    ParsedQuery q; q.type = QueryType::CREATE_TABLE;
    q.table_name = expect(IDENTIFIER).value;
    expect(LPAREN);

    int col_idx = 0;
    do {
        ColDef col;
        col.name = expect(IDENTIFIER).value;

        // Type keyword
        Token tt = consume();
        std::string tp = tt.value;
        if (tp == "VARCHAR") {
            expect(LPAREN);
            tp += "(" + expect(NUMBER_LITERAL).value + ")";
            expect(RPAREN);
        }
        col.type = tp;

        // Optional PRIMARY KEY
        if (check(KW_PRIMARY)) {
            consume();
            expect(KW_KEY);
            col.is_primary_key = true;
            q.primary_key_index = col_idx;
        }

        q.column_defs.push_back(col);
        ++col_idx;
    } while (match(COMMA));

    expect(RPAREN);
    match(SEMICOLON);
    return q;
}

ParsedQuery Parser::parseDropTable() {
    expect(KW_TABLE);
    ParsedQuery q; q.type = QueryType::DROP_TABLE;
    q.table_name = expect(IDENTIFIER).value;
    match(SEMICOLON);
    return q;
}

// ── USE ────────────────────────────────────────────────────────────────

ParsedQuery Parser::parseUse() {
    ParsedQuery q; q.type = QueryType::USE_DATABASE;
    // Allow optional DATABASE keyword
    if (check(KW_DATABASE)) consume();
    q.database_name = expect(IDENTIFIER).value;
    match(SEMICOLON);
    return q;
}

// ── ALTER TABLE ─────────────────────────────────────────────────────────

ParsedQuery Parser::parseAlterTable() {
    expect(KW_TABLE);
    ParsedQuery q; q.type = QueryType::ALTER_TABLE;
    q.table_name = expect(IDENTIFIER).value;
    expect(KW_ADD);
    match(KW_COLUMN); // optional COLUMN keyword
    q.alter_col_name = expect(IDENTIFIER).value;
    // Type keyword
    Token tt = consume();
    std::string tp = tt.value;
    if (tp == "VARCHAR") {
        expect(LPAREN);
        tp += "(" + expect(NUMBER_LITERAL).value + ")";
        expect(RPAREN);
    }
    q.alter_col_type = tp;
    match(SEMICOLON);
    return q;
}

// ── Helper: parse [table.]column ────────────────────────────────────────

QualifiedCol Parser::parseQualifiedCol() {
    QualifiedCol qc;
    qc.column = expect(IDENTIFIER).value;
    if (check(DOT)) {
        consume(); // eat '.'
        qc.table = qc.column;
        qc.column = expect(IDENTIFIER).value;
    }
    return qc;
}

// ── SELECT ─────────────────────────────────────────────────────────────

ParsedQuery Parser::parseSelect() {
    ParsedQuery q; q.type = QueryType::SELECT;
    if (match(STAR)) {
        q.select_all = true;
    } else {
        do {
            SelectColumn sc;
            if (match(KW_COUNT)) { sc.aggr = AggrFunc::COUNT; expect(LPAREN); if(match(STAR)) sc.name = "*"; else sc.name = expect(IDENTIFIER).value; expect(RPAREN); }
            else if (match(KW_SUM)) { sc.aggr = AggrFunc::SUM; expect(LPAREN); sc.name = expect(IDENTIFIER).value; expect(RPAREN); }
            else if (match(KW_AVG)) { sc.aggr = AggrFunc::AVG; expect(LPAREN); sc.name = expect(IDENTIFIER).value; expect(RPAREN); }
            else if (match(KW_MIN)) { sc.aggr = AggrFunc::MIN; expect(LPAREN); sc.name = expect(IDENTIFIER).value; expect(RPAREN); }
            else if (match(KW_MAX)) { sc.aggr = AggrFunc::MAX; expect(LPAREN); sc.name = expect(IDENTIFIER).value; expect(RPAREN); }
            else {
                // Support qualified name: table.column
                sc.name = expect(IDENTIFIER).value;
                if (check(DOT)) {
                    consume();
                    sc.name = sc.name + "." + expect(IDENTIFIER).value;
                }
            }
            if (match(KW_AS)) sc.alias = expect(IDENTIFIER).value;
            q.select_columns.push_back(sc);
        } while (match(COMMA));
    }
    expect(KW_FROM);
    q.table_name = expect(IDENTIFIER).value;

    // ── JOIN clauses ────────────────────────────────────────────────────
    while (true) {
        JoinClause::Type jtype = JoinClause::INNER;
        if (check(KW_INNER)) {
            consume();
            expect(KW_JOIN);
        } else if (check(KW_LEFT)) {
            consume();
            match(KW_OUTER); // optional OUTER
            expect(KW_JOIN);
            jtype = JoinClause::LEFT;
        } else if (check(KW_RIGHT)) {
            consume();
            match(KW_OUTER); // optional OUTER
            expect(KW_JOIN);
            jtype = JoinClause::RIGHT;
        } else if (check(KW_JOIN)) {
            consume(); // bare JOIN = INNER JOIN
        } else {
            break;
        }
        JoinClause jc;
        jc.join_type = jtype;
        jc.table_name = expect(IDENTIFIER).value;
        expect(KW_ON);
        jc.left_col  = parseQualifiedCol();
        expect(OP_EQ);
        jc.right_col = parseQualifiedCol();
        q.joins.push_back(std::move(jc));
    }
    // ── End JOIN ─────────────────────────────────────────────────────────

    if (match(KW_WHERE)) q.where = parseExprOr();
    if (match(KW_GROUP)) {
        expect(KW_BY);
        do {
            q.group_by.push_back(expect(IDENTIFIER).value);
        } while (match(COMMA));
    }
    if (match(KW_HAVING)) q.having = parseExprOr();
    if (match(KW_ORDER)) {
        expect(KW_BY);
        do {
            OrderByClause ob;
            if (match(KW_COUNT)) { ob.aggr = AggrFunc::COUNT; expect(LPAREN); if(match(STAR)) ob.column = "*"; else ob.column = expect(IDENTIFIER).value; expect(RPAREN); }
            else if (match(KW_SUM)) { ob.aggr = AggrFunc::SUM; expect(LPAREN); ob.column = expect(IDENTIFIER).value; expect(RPAREN); }
            else if (match(KW_AVG)) { ob.aggr = AggrFunc::AVG; expect(LPAREN); ob.column = expect(IDENTIFIER).value; expect(RPAREN); }
            else if (match(KW_MIN)) { ob.aggr = AggrFunc::MIN; expect(LPAREN); ob.column = expect(IDENTIFIER).value; expect(RPAREN); }
            else if (match(KW_MAX)) { ob.aggr = AggrFunc::MAX; expect(LPAREN); ob.column = expect(IDENTIFIER).value; expect(RPAREN); }
            else { ob.column = expect(IDENTIFIER).value; }
            if (match(KW_DESC)) ob.asc = false;
            else match(KW_ASC); // optional ASC
            q.order_by.push_back(ob);
        } while (match(COMMA));
    }
    match(SEMICOLON);
    return q;
}

// ── INSERT ─────────────────────────────────────────────────────────────

ParsedQuery Parser::parseInsert() {
    ParsedQuery q; q.type = QueryType::INSERT;
    expect(KW_INTO);
    q.table_name = expect(IDENTIFIER).value;

    // Optional column list
    if (check(LPAREN)) {
        consume();
        do {
            q.insert_columns.push_back(expect(IDENTIFIER).value);
        } while (match(COMMA));
        expect(RPAREN);
    }

    expect(KW_VALUES);

    // Multiple value rows
    do {
        expect(LPAREN);
        std::vector<std::string> vals;
        do {
            Token v = consume();
            vals.push_back(v.value);
        } while (match(COMMA));
        expect(RPAREN);
        q.insert_values.push_back(std::move(vals));
    } while (match(COMMA));

    match(SEMICOLON);
    return q;
}

// ── UPDATE ─────────────────────────────────────────────────────────────

ParsedQuery Parser::parseUpdate() {
    ParsedQuery q; q.type = QueryType::UPDATE;
    q.table_name = expect(IDENTIFIER).value;
    expect(KW_SET);

    do {
        SetClause sc;
        sc.column = expect(IDENTIFIER).value;
        expect(OP_EQ);
        sc.value = consume().value;
        q.set_clauses.push_back(sc);
    } while (match(COMMA));

    if (match(KW_WHERE)) q.where = parseExprOr();
    match(SEMICOLON);
    return q;
}

// ── DELETE ─────────────────────────────────────────────────────────────

ParsedQuery Parser::parseDelete() {
    ParsedQuery q; q.type = QueryType::DELETE_Q;
    expect(KW_FROM);
    q.table_name = expect(IDENTIFIER).value;
    if (match(KW_WHERE)) q.where = parseExprOr();
    match(SEMICOLON);
    return q;
}

// ── WHERE recursive descent (OR < AND < NOT < atom) ────────────────────

std::shared_ptr<WhereExpr> Parser::parseExprOr() {
    auto left = parseExprAnd();
    while (match(KW_OR)) {
        auto node = std::make_shared<WhereExpr>();
        node->kind = WhereExpr::OR_OP;
        node->left = left;
        node->right = parseExprAnd();
        left = node;
    }
    return left;
}

std::shared_ptr<WhereExpr> Parser::parseExprAnd() {
    auto left = parseExprNot();
    while (match(KW_AND)) {
        auto node = std::make_shared<WhereExpr>();
        node->kind = WhereExpr::AND_OP;
        node->left = left;
        node->right = parseExprNot();
        left = node;
    }
    return left;
}

std::shared_ptr<WhereExpr> Parser::parseExprNot() {
    if (match(KW_NOT)) {
        auto node = std::make_shared<WhereExpr>();
        node->kind = WhereExpr::NOT_OP;
        node->left = parseExprNot();
        return node;
    }
    return parseExprAtom();
}

std::shared_ptr<WhereExpr> Parser::parseExprAtom() {
    if (match(LPAREN)) {
        auto expr = parseExprOr();
        expect(RPAREN);
        return expr;
    }

    // comparison: column op value
    auto node = std::make_shared<WhereExpr>();
    node->kind = WhereExpr::CMP;

    if (match(KW_COUNT)) { node->aggr = AggrFunc::COUNT; expect(LPAREN); if(match(STAR)) node->column = "*"; else node->column = expect(IDENTIFIER).value; expect(RPAREN); }
    else if (match(KW_SUM)) { node->aggr = AggrFunc::SUM; expect(LPAREN); node->column = expect(IDENTIFIER).value; expect(RPAREN); }
    else if (match(KW_AVG)) { node->aggr = AggrFunc::AVG; expect(LPAREN); node->column = expect(IDENTIFIER).value; expect(RPAREN); }
    else if (match(KW_MIN)) { node->aggr = AggrFunc::MIN; expect(LPAREN); node->column = expect(IDENTIFIER).value; expect(RPAREN); }
    else if (match(KW_MAX)) { node->aggr = AggrFunc::MAX; expect(LPAREN); node->column = expect(IDENTIFIER).value; expect(RPAREN); }
    else { node->column = expect(IDENTIFIER).value; }

    Token opTok = consume();
    switch (opTok.type) {
        case OP_EQ:  node->op = "=";  break;
        case OP_NEQ: node->op = "!="; break;
        case OP_LT:  node->op = "<";  break;
        case OP_GT:  node->op = ">";  break;
        case OP_LTE: node->op = "<="; break;
        case OP_GTE: node->op = ">="; break;
        default: throw std::runtime_error("Expected comparison operator, got: " + opTok.value);
    }

    node->value = consume().value;
    return node;
}

} // namespace db
