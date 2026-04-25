#include "../include/type_inference.h"
#include <cstdlib>
#include <cerrno>
#include <cctype>

bool TypeInference::isInt64(const std::string& str) {
    if (str.empty()) return false;

    char* endptr;
    errno = 0;
    long long val = std::strtoll(str.c_str(), &endptr, 10);

    (void)val;

    return (errno == 0 && *endptr == '\0');
}

bool TypeInference::isDouble(const std::string& str) {
    if (str.empty()) return false;

    char* endptr;
    errno = 0;
    double val = std::strtod(str.c_str(), &endptr);

    (void)val;

    return (errno == 0 && *endptr == '\0');
}

ColumnType TypeInference::inferType(const std::vector<std::string>& values) {
    if (values.empty()) {
        return ColumnType::STRING;
    }

    bool all_int    = true;
    bool all_double = true;

    for (const auto& v : values) {
        // FIXED (Warning 2 fix): skip empty cells when inferring type
        // Before this fix: empty string made isInt64() and isDouble()
        // both return false, so a column like:
        //   price: 99.99, 149.50, [empty], 79.95
        // would be inferred as STRING even though it's clearly DOUBLE.
        // The loader then tried to write it as a string but substituted
        // 0.0 for the empty cell — the inferred type and written type
        // contradicted each other.
        // Fix: skip empty strings. An empty cell doesn't tell us anything
        // about the column's type. The loader already handles empty cells
        // by writing 0 or 0.0 as a default value.
        if (v.empty()) continue;

        if (!isInt64(v))  all_int    = false;
        if (!isDouble(v)) all_double = false;

        if (!all_int && !all_double) {
            return ColumnType::STRING;
        }
    }

    // Edge case: if EVERY cell was empty, default to STRING
    if (all_int)    return ColumnType::INT64;
    if (all_double) return ColumnType::DOUBLE;
    return ColumnType::STRING;
}

std::vector<ColumnType> TypeInference::inferAllTypes(
    const std::vector<Row>& rows,
    size_t num_columns)
{
    std::vector<ColumnType> types(num_columns, ColumnType::STRING);

    if (rows.empty()) {
        return types;
    }

    for (size_t col = 0; col < num_columns; col++) {
        std::vector<std::string> column_values;
        for (const auto& row : rows) {
            if (col < row.size()) {
                column_values.push_back(row[col]);
            } else {
                column_values.push_back("");
            }
        }
        types[col] = inferType(column_values);
    }

    return types;
}
