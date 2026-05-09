#include "../include/type_inference.h"
#include <cstdlib>
#include <cerrno>
#include <climits>
#include <cctype>

// ------------------------------------------------------------
// Check INT64
// ------------------------------------------------------------
bool TypeInference::isInt64(const std::string& str) {
    if (str.empty()) return false;

    char* endptr;
    errno = 0;
    long long val = std::strtoll(str.c_str(), &endptr, 10);
    (void)val;

    return (errno == 0 && *endptr == '\0');
}

// ------------------------------------------------------------
// Check DOUBLE
// ------------------------------------------------------------
bool TypeInference::isDouble(const std::string& str) {
    if (str.empty()) return false;

    char* endptr;
    errno = 0;
    double val = std::strtod(str.c_str(), &endptr);
    (void)val;

    return (errno == 0 && *endptr == '\0');
}

// ------------------------------------------------------------
// CORE TYPE INFERENCE
// Priority:
//   INT32  < INT64 < DOUBLE < STRING
//
// FIX (Issue 7): The all_empty check was previously placed after
// the main loop but logically belonged inside it. The problem:
// if every value is empty, the loop's early-exit (`if (!all_int &&
// !all_double) return STRING`) never fires because empty strings
// are skipped with `continue`, leaving all_int and all_double both
// true. The code then falls through to the INT32/INT64/DOUBLE
// checks and would (incorrectly) return INT32 for an all-empty
// column, because min_val stays LLONG_MAX and max_val stays
// LLONG_MIN, which fits in INT32.
//
// Fix: track whether any non-empty value was seen inside the main
// loop (via `seen_any`). After the loop, if no real value was seen,
// return STRING immediately. This is cleaner than the post-loop
// all_empty scan in the original code.
// ------------------------------------------------------------
ColumnType TypeInference::inferType(const std::vector<std::string>& values) {
    if (values.empty()) return ColumnType::STRING;

    bool all_int    = true;
    bool all_double = true;
    bool seen_any   = false;   // FIX (Issue 7): track whether any non-empty value was seen

    long long min_val = LLONG_MAX;
    long long max_val = LLONG_MIN;

    for (const auto& v : values) {
        if (v.empty()) continue;   // skip missing cells

        seen_any = true;           // FIX (Issue 7): mark that we have at least one value

        if (isInt64(v)) {
            long long num = std::stoll(v);
            if (num < min_val) min_val = num;
            if (num > max_val) max_val = num;
        } else {
            all_int = false;
        }

        if (!isDouble(v)) {
            all_double = false;
        }

        if (!all_int && !all_double) {
            return ColumnType::STRING;
        }
    }

    // FIX (Issue 7): if every cell was empty (or the input only had empty
    // strings), treat the column as STRING — we have no type evidence.
    if (!seen_any) return ColumnType::STRING;

    if (all_int) {
        if (min_val >= INT32_MIN && max_val <= INT32_MAX) {
            return ColumnType::INT32;
        }
        return ColumnType::INT64;
    }

    if (all_double) {
        return ColumnType::DOUBLE;
    }

    return ColumnType::STRING;
}

// ------------------------------------------------------------
// Infer all columns
// ------------------------------------------------------------
std::vector<ColumnType> TypeInference::inferAllTypes(
    const std::vector<Row>& rows,
    size_t num_columns)
{
    std::vector<ColumnType> types(num_columns, ColumnType::STRING);

    if (rows.empty()) return types;

    for (size_t col = 0; col < num_columns; col++) {
        std::vector<std::string> column_values;

        for (const auto& row : rows) {
            if (col < row.size()) {
                column_values.push_back(row[col]);
            } else {
                column_values.push_back(""); // missing cell
            }
        }

        types[col] = inferType(column_values);
    }

    return types;
}