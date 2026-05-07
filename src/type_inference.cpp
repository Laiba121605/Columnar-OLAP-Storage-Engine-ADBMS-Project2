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
// ------------------------------------------------------------
ColumnType TypeInference::inferType(const std::vector<std::string>& values) {
    if (values.empty()) return ColumnType::STRING;

    bool all_int = true;
    bool all_double = true;

    long long min_val = LLONG_MAX;
    long long max_val = LLONG_MIN;

    for (const auto& v : values) {
        if (v.empty()) continue;

        if (isInt64(v)) {
            long long num = std::stoll(v);
            min_val = std::min(min_val, num);
            max_val = std::max(max_val, num);
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

    // ========== ADD THIS BLOCK HERE ==========
    bool all_empty = true;
    for (const auto& v : values) {
        if (!v.empty()) { all_empty = false; break; }
    }
    if (all_empty) return ColumnType::STRING;
    // =========================================

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