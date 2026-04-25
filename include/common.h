#ifndef COMMON_H
#define COMMON_H

#include <cstdint>
#include <string>
#include <vector>

enum class ColumnType : uint8_t {
    INT64 = 0,
    DOUBLE = 1,
    STRING = 2
};

enum class Encoding : uint8_t {
    NONE = 0,
    DICTIONARY = 1,
    RLE = 2
};

#pragma pack(push, 1)
struct ColumnHeader {
    uint32_t magic;        // 0x434F4C44 ("COLD")
    uint8_t version;       // 1
    uint8_t type;          // ColumnType value
    uint8_t encoding;      // Encoding value  
    uint8_t reserved;      // 0 for now
    uint64_t row_count;    // Number of rows
    uint64_t data_offset;  // Where values start (always 32)
};
#pragma pack(pop)

using Row = std::vector<std::string>;

constexpr uint32_t MAGIC = 0x434F4C44;
constexpr uint8_t VERSION = 1;
constexpr size_t HEADER_SIZE = 32;

const char* typeToString(ColumnType type);
ColumnType stringToType(const std::string& str);
const char* encodingToString(Encoding enc);

#endif // COMMON_H