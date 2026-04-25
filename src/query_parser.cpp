#include "../include/query_parser.h"
#include <iostream>
#include <sstream>
#include <algorithm>
#include <cctype>

// ============================================================
// QueryParser implementation
// Hand-written recursive descent parser per manual Section 7.
// ============================================================

QueryParser::QueryParser(const std::string& query_text)
    : text_(query_text), pos_(0) {
    tokenize();
}

// ============================================================
// tokenize()
// Splits query text into tokens. Handles:
//   - keywords and identifiers (split on whitespace)
//   - operators: >=, <=, !=, =, <, >
//   - parentheses: ( )
//   - commas: ,
//   - string literals: 'value' → stored as value (quotes stripped)
//   - numeric literals: stored as-is
//
// Example:
//   "SELECT SUM(price) FROM sales WHERE date >= 20240101"
//   → ["SELECT","SUM","(","price",")","FROM","sales",
//      "WHERE","date",">=","20240101"]
// ============================================================
void QueryParser::tokenize() {
    tokens_.clear();
    size_t i = 0;

    while (i < text_.size()) {
        char c = text_[i];

        // Skip whitespace
        if (isspace(c)) { i++; continue; }

        // String literal: 'value'
        if (c == '\'') {
            i++;
            std::string lit;
            while (i < text_.size() && text_[i] != '\'') {
                lit += text_[i++];
            }
            if (i < text_.size()) i++; // skip closing '
            tokens_.push_back(lit);
            continue;
        }

        // Two-character operators: >=, <=, !=
        if (i + 1 < text_.size()) {
            std::string two = text_.substr(i, 2);
            if (two == ">=" || two == "<=" || two == "!=") {
                tokens_.push_back(two);
                i += 2;
                continue;
            }
        }

        // Single-character tokens
        if (c == '(' || c == ')' || c == ',' || c == '*' ||
            c == '=' || c == '<' || c == '>') {
            tokens_.push_back(std::string(1, c));
            i++;
            continue;
        }

        // Identifier or number: read until whitespace or special char
        if (isalnum(c) || c == '_' || c == '.' || c == '-') {
            std::string tok;
            while (i < text_.size() &&
                   (isalnum(text_[i]) || text_[i] == '_' ||
                    text_[i] == '.' || text_[i] == '-')) {
                tok += text_[i++];
            }
            tokens_.push_back(tok);
            continue;
        }

        // Skip unknown characters
        i++;
    }
}

// ── Token helpers ────────────────────────────────────────────

std::string QueryParser::peek() const {
    if (pos_ >= tokens_.size()) return "";
    return toUpper(tokens_[pos_]);
}

std::string QueryParser::consume() {
    if (pos_ >= tokens_.size()) return "";
    return tokens_[pos_++];
}

bool QueryParser::expect(const std::string& tok) {
    if (toUpper(peek()) != toUpper(tok)) {
        error_ = "Expected '" + tok + "' but got '" + peek() + "'";
        return false;
    }
    consume();
    return true;
}

bool QueryParser::atEnd() const {
    return pos_ >= tokens_.size();
}

std::string QueryParser::toUpper(const std::string& s) {
    std::string r = s;
    for (char& c : r) c = toupper(c);
    return r;
}

bool QueryParser::isAggFunc(const std::string& s) {
    std::string u = s;
    for (char& c : u) c = toupper(c);
    return u == "COUNT" || u == "SUM" || u == "AVG" ||
           u == "MIN"   || u == "MAX";
}

AggFunc QueryParser::toAggFunc(const std::string& s) {
    std::string u = s;
    for (char& c : u) c = toupper(c);
    if (u == "COUNT") return AggFunc::COUNT;
    if (u == "SUM")   return AggFunc::SUM;
    if (u == "AVG")   return AggFunc::AVG;
    if (u == "MIN")   return AggFunc::MIN;
    if (u == "MAX")   return AggFunc::MAX;
    return AggFunc::NONE;
}

bool QueryParser::isOp(const std::string& s) {
    return s == "=" || s == "<" || s == "<=" ||
           s == ">" || s == ">=" || s == "!=";
}

// ============================================================
// parse() — top-level entry point
// Grammar: SELECT ... FROM ... [WHERE ...] [GROUP BY ...]
// ============================================================
bool QueryParser::parse(QueryPlan& plan) {
    plan = QueryPlan{};
    pos_ = 0;
    error_ = "";

    if (atEnd()) {
        error_ = "Empty query";
        return false;
    }

    // Must start with SELECT
    if (!parseSelect(plan))   return false;
    if (!parseFrom(plan))     return false;
    if (!atEnd() && peek() == "WHERE")    parseWhere(plan);
    if (!atEnd() && peek() == "GROUP")    parseGroupBy(plan);

    return true;
}

// ============================================================
// parseSelect()
// Handles: SELECT * | SELECT expr [, expr ...]
// ============================================================
bool QueryParser::parseSelect(QueryPlan& plan) {
    if (!expect("SELECT")) return false;

    // SELECT *
    if (peek() == "*") {
        consume();
        plan.select_star = true;
        return true;
    }

    return parseSelectList(plan);
}

// ============================================================
// parseSelectList()
// Handles comma-separated list of SelectExprs
// ============================================================
bool QueryParser::parseSelectList(QueryPlan& plan) {
    SelectExpr expr;
    if (!parseSelectExpr(expr)) return false;
    plan.selects.push_back(expr);

    while (!atEnd() && peek() == ",") {
        consume(); // eat comma
        SelectExpr next_expr;
        if (!parseSelectExpr(next_expr)) return false;
        plan.selects.push_back(next_expr);
    }
    return true;
}

// ============================================================
// parseSelectExpr()
// Handles:
//   plain column:   "country"
//   aggregate:      "SUM(price)", "COUNT(*)", "AVG(quantity)"
// ============================================================
bool QueryParser::parseSelectExpr(SelectExpr& expr) {
    std::string tok = peek();

    if (isAggFunc(tok)) {
        // Aggregate function
        expr.agg = toAggFunc(consume());

        if (!expect("(")) return false;

        if (peek() == "*") {
            // COUNT(*)
            consume();
            expr.star = true;
            expr.col  = "";
        } else {
            // SUM(col), AVG(col), etc.
            expr.col  = consume();
            expr.star = false;
        }

        if (!expect(")")) return false;
    } else {
        // Plain column reference
        expr.agg  = AggFunc::NONE;
        expr.col  = consume();
        expr.star = false;

        if (expr.col.empty()) {
            error_ = "Expected column name in SELECT";
            return false;
        }
    }
    return true;
}

// ============================================================
// parseFrom()
// Handles: FROM table_name
// ============================================================
bool QueryParser::parseFrom(QueryPlan& plan) {
    if (!expect("FROM")) return false;

    if (atEnd() || peek() == "WHERE" || peek() == "GROUP") {
        error_ = "Expected table name after FROM";
        return false;
    }

    plan.table = consume();
    return true;
}

// ============================================================
// parseWhere()
// Phase 1: single predicate only — col op literal
// Example: "WHERE date >= 20240101"
//          "WHERE category = 'Electronics'"
// ============================================================
bool QueryParser::parseWhere(QueryPlan& plan) {
    if (!expect("WHERE")) return false;

    plan.where.has_where = true;

    // Column name
    if (atEnd()) { error_ = "Expected column after WHERE"; return false; }
    plan.where.col = consume();

    // Operator
    if (atEnd() || !isOp(peek())) {
        error_ = "Expected operator after WHERE column";
        return false;
    }
    plan.where.op = consume();

    // Literal value
    if (atEnd()) { error_ = "Expected value after WHERE operator"; return false; }
    plan.where.val = consume();

    return true;
}

// ============================================================
// parseGroupBy()
// Parsed in Phase 1, but executor only uses it in Phase 2.
// ============================================================
bool QueryParser::parseGroupBy(QueryPlan& plan) {
    if (!expect("GROUP")) return false;
    if (!expect("BY"))    return false;

    if (atEnd()) { error_ = "Expected column after GROUP BY"; return false; }
    plan.groupby = consume();

    // TODO Phase 2: executor will use plan.groupby for hash map aggregation
    return true;
}

// ============================================================
// neededColumns()
// Returns all column names this query actually touches.
// The executor calls this to open ONLY those .col files.
// This is what makes columnar storage faster — we skip columns
// that aren't needed by the query.
//
// Example: SELECT country, SUM(price) FROM sales WHERE date >= 20240101
//   → needs: {"country", "price", "date"}
//   → does NOT open: id.col, quantity.col, product_id.col
// ============================================================
std::vector<std::string> QueryPlan::neededColumns() const {
    std::vector<std::string> cols;
    auto add = [&](const std::string& c) {
        if (!c.empty()) {
            // avoid duplicates
            for (auto& existing : cols)
                if (existing == c) return;
            cols.push_back(c);
        }
    };

    if (select_star) return cols; // executor will read schema for SELECT *

    for (const auto& sel : selects) {
        if (!sel.star) add(sel.col);
    }
    if (where.has_where) add(where.col);
    if (!groupby.empty()) add(groupby);

    return cols;
}

// ============================================================
// printQueryPlan() — debug helper
// ============================================================
void printQueryPlan(const QueryPlan& plan) {
    std::cout << "QueryPlan {\n";
    std::cout << "  table:  " << plan.table << "\n";

    if (plan.select_star) {
        std::cout << "  select: *\n";
    } else {
        for (const auto& s : plan.selects) {
            std::cout << "  select: ";
            if (s.agg != AggFunc::NONE) {
                switch (s.agg) {
                    case AggFunc::COUNT: std::cout << "COUNT"; break;
                    case AggFunc::SUM:   std::cout << "SUM";   break;
                    case AggFunc::AVG:   std::cout << "AVG";   break;
                    case AggFunc::MIN:   std::cout << "MIN";   break;
                    case AggFunc::MAX:   std::cout << "MAX";   break;
                    default: break;
                }
                std::cout << "(" << (s.star ? "*" : s.col) << ")";
            } else {
                std::cout << s.col;
            }
            std::cout << "\n";
        }
    }

    if (plan.where.has_where) {
        std::cout << "  where:  " << plan.where.col << " "
                  << plan.where.op << " " << plan.where.val << "\n";
    }
    if (!plan.groupby.empty()) {
        std::cout << "  groupby: " << plan.groupby << "\n";
    }

    auto needed = plan.neededColumns();
    std::cout << "  opens cols: [";
    for (size_t i = 0; i < needed.size(); i++) {
        std::cout << needed[i];
        if (i + 1 < needed.size()) std::cout << ", ";
    }
    std::cout << "]\n}\n";
}
