#include "../include/common.h"

const char* typeToString(ColumnType type) {
    switch (type) {
        case ColumnType::INT64:  return "int64";
        case ColumnType::DOUBLE: return "double";
        case ColumnType::STRING: return "string";
        default: return "unknown";
    }
}

ColumnType stringToType(const std::string& str) {
    if (str == "int64") return ColumnType::INT64;
    if (str == "double") return ColumnType::DOUBLE;
    return ColumnType::STRING;
}

const char* encodingToString(Encoding enc) {
    switch (enc) {
        case Encoding::NONE:       return "none";
        case Encoding::DICTIONARY: return "dictionary";
        case Encoding::RLE:        return "rle";
        default: return "unknown";
    }
}