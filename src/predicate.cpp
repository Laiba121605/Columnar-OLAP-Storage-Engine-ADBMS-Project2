#include "../include/predicate.h"
#include <iostream>
#include <stdexcept>

std::string PredicateEvaluator::last_error_ = "";

// ============================================================
// evaluate()
// Opens the WHERE column itself, delegates to evaluateWithReader.
// ============================================================
Bitmap PredicateEvaluator::evaluate(const WherePredicate& pred,
                                    const std::string& table_dir,
                                    uint64_t row_count) {
    // No WHERE clause — every row passes
    if (!pred.has_where) {
        return Bitmap(row_count, true);
    }

    ColumnReader reader;
    if (!reader.open(table_dir, pred.col)) {
        last_error_ = "Cannot open WHERE column '" + pred.col +
                      "': " + reader.getLastError();
        return Bitmap();
    }

    Bitmap result = evaluateWithReader(pred, reader);
    reader.close();
    return result;
}

// ============================================================
// evaluateWithReader()
// Core evaluation loop. Walks every row in the column,
// applies the predicate, sets bitmap[i] accordingly.
//
// Manual Section 6.2:
// "a bit vector of length N where bit i is 1 if row i passed"
// ============================================================
Bitmap PredicateEvaluator::evaluateWithReader(const WherePredicate& pred,
                                              ColumnReader& reader) {
    if (!pred.has_where) {
        return Bitmap(reader.getRowCount(), true);
    }

    Bitmap bitmap;
    bitmap.reserve(reader.getRowCount());

    while (reader.hasNext()) {
        ColumnValue val = reader.next();
        bool passes = false;

        // Dispatch comparison based on column type
        // Manual Section 6.2: "column OP value where OP is
        // one of =, <, <=, >, >=, !="
        if (std::holds_alternative<int64_t>(val)) {
            passes = compareInt64(std::get<int64_t>(val),
                                  pred.op, pred.val);
        } else if (std::holds_alternative<double>(val)) {
            passes = compareDouble(std::get<double>(val),
                                   pred.op, pred.val);
        } else if (std::holds_alternative<std::string>(val)) {
            passes = compareString(std::get<std::string>(val),
                                   pred.op, pred.val);
        }

        bitmap.push_back(passes);
    }

    // Reset reader so P3 can reuse it for the aggregate scan
    reader.reset();
    return bitmap;
}

// ============================================================
// compareInt64()
// Converts the literal string to int64 and compares.
// ============================================================
bool PredicateEvaluator::compareInt64(int64_t val,
                                      const std::string& op,
                                      const std::string& literal) {
    int64_t rhs = 0;
    try { rhs = std::stoll(literal); }
    catch (...) { return false; }

    if (op == "=")  return val == rhs;
    if (op == "!=") return val != rhs;
    if (op == "<")  return val <  rhs;
    if (op == "<=") return val <= rhs;
    if (op == ">")  return val >  rhs;
    if (op == ">=") return val >= rhs;
    return false;
}

// ============================================================
// compareDouble()
// Converts the literal string to double and compares.
// ============================================================
bool PredicateEvaluator::compareDouble(double val,
                                       const std::string& op,
                                       const std::string& literal) {
    double rhs = 0.0;
    try { rhs = std::stod(literal); }
    catch (...) { return false; }

    if (op == "=")  return val == rhs;
    if (op == "!=") return val != rhs;
    if (op == "<")  return val <  rhs;
    if (op == "<=") return val <= rhs;
    if (op == ">")  return val >  rhs;
    if (op == ">=") return val >= rhs;
    return false;
}

// ============================================================
// compareString()
// Lexicographic string comparison.
// ============================================================
bool PredicateEvaluator::compareString(const std::string& val,
                                       const std::string& op,
                                       const std::string& literal) {
    if (op == "=")  return val == literal;
    if (op == "!=") return val != literal;
    if (op == "<")  return val <  literal;
    if (op == "<=") return val <= literal;
    if (op == ">")  return val >  literal;
    if (op == ">=") return val >= literal;
    return false;
}

// ============================================================
// countTrue()
// Counts rows that passed the filter.
// P3 uses this directly for COUNT(*) queries — no need
// to open any aggregate column at all.
// ============================================================
uint64_t PredicateEvaluator::countTrue(const Bitmap& bitmap) {
    uint64_t count = 0;
    for (bool b : bitmap) if (b) count++;
    return count;
}

// ============================================================
// printBitmap() — debug helper
// ============================================================
void PredicateEvaluator::printBitmap(const Bitmap& bitmap) {
    std::cout << "Bitmap [";
    for (size_t i = 0; i < bitmap.size(); i++) {
        std::cout << (bitmap[i] ? "1" : "0");
        if (i + 1 < bitmap.size()) std::cout << ",";
    }
    std::cout << "] (" << countTrue(bitmap)
              << "/" << bitmap.size() << " rows pass)\n";
}
