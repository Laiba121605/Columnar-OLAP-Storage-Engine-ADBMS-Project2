#ifndef PREDICATE_H
#define PREDICATE_H

// ============================================================
// predicate.h — P2: Predicate Evaluator
// ============================================================
// Takes the WherePredicate from the query parser and a
// ColumnReader, scans the WHERE column, and produces a bitmap.
//
// Manual Section 6.2:
// "The executor reads the predicate's column and walks it,
//  producing a bitmap of which row positions satisfy the
//  predicate: a bit vector of length N where bit i is 1 if
//  row i passed the filter."
//
// The bitmap is then handed to P3's aggregator which uses it
// to skip rows that didn't pass the filter — P3 never does
// any comparison logic, they just check the bitmap.
//
// Example:
//   WHERE date >= 20240101 on 5 rows:
//   row 0: date=20231201 → false
//   row 1: date=20240115 → true
//   row 2: date=20240203 → true
//   row 3: date=20231105 → false
//   row 4: date=20240301 → true
//   bitmap = [false, true, true, false, true]
// ============================================================

#include "common.h"
#include "query_parser.h"
#include "column_reader.h"
#include <vector>
#include <string>

// ── Bitmap type ─────────────────────────────────────────────
// Simple vector<bool>. Index i = true means row i passed WHERE.
// P3 iterates over aggregate columns and checks bitmap[i]
// before including a row in the result.
using Bitmap = std::vector<bool>;

// ── PredicateEvaluator ──────────────────────────────────────
class PredicateEvaluator {
public:
    // Evaluate a WHERE predicate against a column.
    // Opens the WHERE column from table_dir, reads every value,
    // compares against pred.val using pred.op, builds bitmap.
    //
    // Returns an ALL-TRUE bitmap if pred.has_where == false
    // (no WHERE clause = every row passes).
    //
    // Returns empty bitmap on error — check getLastError().
    static Bitmap evaluate(const WherePredicate& pred,
                           const std::string& table_dir,
                           uint64_t row_count);

    // Same but uses an already-open ColumnReader.
    // Reader must be freshly opened or reset() before calling.
    // Reader is reset() after evaluation so P3 can reuse it.
    static Bitmap evaluateWithReader(const WherePredicate& pred,
                                     ColumnReader& reader);

    static std::string getLastError() { return last_error_; }

    // Utility: count how many rows passed the filter.
    // Useful for COUNT(*) — just return popcount(bitmap).
    static uint64_t countTrue(const Bitmap& bitmap);

    // Utility: print bitmap for debugging
    static void printBitmap(const Bitmap& bitmap);

private:
    static std::string last_error_;

    // Type-specific comparators
    static bool compareInt64 (int64_t     val, const std::string& op, const std::string& literal);
    static bool compareDouble(double      val, const std::string& op, const std::string& literal);
    static bool compareString(const std::string& val, const std::string& op, const std::string& literal);
};

#endif // PREDICATE_H
