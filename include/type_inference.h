#ifndef TYPE_INFERENCE_H
#define TYPE_INFERENCE_H

#include "common.h"
#include <string>
#include <vector>

class TypeInference {
public:
    static ColumnType inferType(const std::vector<std::string>& column_values);
    
    static std::vector<ColumnType> inferAllTypes(
        const std::vector<Row>& rows,
        size_t num_columns
    );
    
    static bool isInt64(const std::string& str);
    static bool isDouble(const std::string& str);
};

#endif // TYPE_INFERENCE_H