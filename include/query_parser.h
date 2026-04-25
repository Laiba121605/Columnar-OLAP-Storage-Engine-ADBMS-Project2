#ifndef QUERY_PARSER_H
#define QUERY_PARSER_H

// ============================================================
// query_parser.h — P2: Query DSL Parser
// ============================================================
// Parses the tiny SQL subset defined in manual Section 7.
// Phase 1 subset (Section 8):
//   SELECT col [, col] | COUNT(*) | SUM(col)
//   FROM table_name
//   [WHERE col op literal]       ← single predicate only
//   [GROUP BY col]               ← parsed but executor handles in Phase 2
//
// Produces a QueryPlan struct that P3's executor consumes.
// P3 never touches raw query text — everything goes through here.
//
// Example:
//   Input:  "SELECT country, SUM(price) FROM sales WHERE date >= 20240101"
//   Output: QueryPlan {
//               table    = "sales"
//               selects  = [{col="country"}, {agg=SUM, col="price"}]
//               where    = {col="date", op=">=", val="20240101"}
//           }
// ============================================================

#include <string>
#include <vector>

// ── AggFunc ─────────────────────────────────────────────────
// Which aggregate function is being used, if any.
enum class AggFunc {
    NONE,   // plain column reference e.g. SELECT country
    COUNT,  // COUNT(*) or COUNT(col)
    SUM,    // SUM(col)
    AVG,    // AVG(col)
    MIN,    // MIN(col)
    MAX     // MAX(col)
};

// ── SelectExpr ──────────────────────────────────────────────
// One item in the SELECT list.
// Examples:
//   "country"     → {agg=NONE,  col="country", star=false}
//   "SUM(price)"  → {agg=SUM,   col="price",   star=false}
//   "COUNT(*)"    → {agg=COUNT, col="",         star=true}
struct SelectExpr {
    AggFunc     agg  = AggFunc::NONE;
    std::string col  = "";
    bool        star = false;   // true only for COUNT(*)
};

// ── WherePredicate ──────────────────────────────────────────
// Single WHERE condition. Phase 1 supports only one.
// Phase 2 bonus: multiple predicates with AND/OR.
// Example: "date >= 20240101"
//   col="date"  op=">="  val="20240101"  has_where=true
struct WherePredicate {
    bool        has_where = false;
    std::string col       = "";
    std::string op        = "";   // =, <, <=, >, >=, !=
    std::string val       = "";   // raw string — executor converts to right type
};

// ── QueryPlan ───────────────────────────────────────────────
// The fully parsed query. P3's executor receives this and runs it.
// This is the contract between P2 and P3.
struct QueryPlan {
    std::string              table;      // FROM table_name
    std::vector<SelectExpr>  selects;    // SELECT list
    WherePredicate           where;      // WHERE clause (optional)
    std::string              groupby;    // GROUP BY col (optional, Phase 2)
    bool                     select_star = false; // SELECT *

    // Convenience: collect all column names this query actually needs.
    // The executor uses this to open only those .col files.
    // This is the columnar advantage — don't open what you don't need.
    std::vector<std::string> neededColumns() const;
};

// ── QueryParser ─────────────────────────────────────────────
// Hand-written recursive descent parser (~100 lines per manual Section 7).
// No external parser library used (forbidden by Section 13).
//
// Usage:
//   QueryParser p("SELECT SUM(price) FROM sales WHERE date >= 20240101");
//   QueryPlan plan;
//   if (p.parse(plan)) { /* hand plan to P3 */ }
//   else { cerr << p.getError(); }
class QueryParser {
public:
    explicit QueryParser(const std::string& query_text);

    // Parse the query. Returns true on success, false on syntax error.
    // On success, plan is populated. On failure, getError() explains why.
    bool parse(QueryPlan& plan);

    std::string getError() const { return error_; }

private:
    std::string              text_;     // original query text
    std::vector<std::string> tokens_;   // tokenized words/symbols
    size_t                   pos_;      // current token position
    std::string              error_;    // last error message

    // ── Tokenizer ───────────────────────────────────────────
    void tokenize();

    // ── Token helpers ───────────────────────────────────────
    std::string peek() const;
    std::string consume();
    bool        expect(const std::string& tok);
    bool        atEnd() const;

    // ── Grammar rules ───────────────────────────────────────
    bool parseSelect(QueryPlan& plan);
    bool parseSelectList(QueryPlan& plan);
    bool parseSelectExpr(SelectExpr& expr);
    bool parseFrom(QueryPlan& plan);
    bool parseWhere(QueryPlan& plan);
    bool parseGroupBy(QueryPlan& plan);

    // ── Helpers ─────────────────────────────────────────────
    static std::string toUpper(const std::string& s);
    static bool        isAggFunc(const std::string& s);
    static AggFunc     toAggFunc(const std::string& s);
    static bool        isOp(const std::string& s);
};

// Helper to print a QueryPlan (useful for debugging)
void printQueryPlan(const QueryPlan& plan);

#endif // QUERY_PARSER_H
