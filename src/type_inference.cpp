#include "../include/type_inference.h"
#include <cstdlib>
#include <cerrno>
#include <cctype>

bool TypeInference::isInt64(const std::string& str) {
    if (str.empty()) return false;
    
    char* endptr;
    errno = 0;
    long long val = std::strtoll(str.c_str(), &endptr, 10);
    
    (void)val; // unused
    
    return (errno == 0 && *endptr == '\0');
}

bool TypeInference::isDouble(const std::string& str) {
    if (str.empty()) return false;
    
    char* endptr;
    errno = 0;
    double val = std::strtod(str.c_str(), &endptr);
    
    (void)val; // unused
    
    return (errno == 0 && *endptr == '\0');
}

ColumnType TypeInference::inferType(const std::vector<std::string>& values) {
    if (values.empty()) {
        return ColumnType::STRING;
    }
    
    bool all_int = true;
    bool all_double = true;
    
    for (const auto& v : values) {
        if (v.empty()) {
            all_int = false;
            all_double = false;
            continue;
        }
        
        if (!isInt64(v)) {
            all_int = false;
        }
        
        if (!isDouble(v)) {
            all_double = false;
        }
        
        if (!all_int && !all_double) {
            return ColumnType::STRING;
        }
    }
    
    if (all_int) return ColumnType::INT64;
    if (all_double) return ColumnType::DOUBLE;
    return ColumnType::STRING;
}

std::vector<ColumnType> TypeInference::inferAllTypes(
    const std::vector<Row>& rows,
    size_t num_columns) {
    
    std::vector<ColumnType> types(num_columns, ColumnType::STRING);
    
    if (rows.empty()) {
        return types;
    }
    
    // For each column, collect all values
    for (size_t col = 0; col < num_columns; col++) {
        std::vector<std::string> column_values;
        for (const auto& row : rows) {
            if (col < row.size()) {
                column_values.push_back(row[col]);
            } else {
                column_values.push_back(""); // empty for missing
            }
        }
        types[col] = inferType(column_values);
    }
    
    return types;
}