#ifndef COMMON_H
#define COMMON_H

#include <cstdint>
#include <string>
#include <vector>

// ============================================================
// ColumnType enum
// Values match the manual spec exactly (Section 4):
//   1=INT32, 2=INT64, 3=DOUBLE, 4=STRING
// These values are written as raw bytes into every .col file.
// Changing them means all existing .col files must be regenerated.
//
// TODO (Phase 2/3): INT32 is here for date columns and other
// 32-bit integer columns when needed.
// ============================================================
enum class ColumnType : uint8_t {
    INT32  = 1,
    INT64  = 2,
    DOUBLE = 3,
    STRING = 4
};

// ============================================================
// Encoding enum
// Values 0,1,2 match the manual (Section 4).
// TODO (Phase 2): DICTIONARY and RLE used when encoding
// selection logic is added to the loader.
// ============================================================
enum class Encoding : uint8_t {
    NONE       = 0,
    DICTIONARY = 1,
    RLE        = 2
};

// ============================================================
// ColumnHeader struct — matches manual spec Section 4 exactly:
//
//   Offset  Size  Field
//   0       4     magic      uint32  ASCII 'C','O','L','1'
//   4       4     version    uint32  must be 1
//   8       1     type       uint8   ColumnType value
//   9       1     encoding   uint8   Encoding value
//   10      2     reserved   uint16  must be 0
//   12      8     row_count  uint64  total rows
//   20      8     data_size  uint64  size of data section in bytes
//   28      8     dict_size  uint64  size of dict section (0 in Phase 1)
//   Total: 36 bytes
//
// #pragma pack(1) ensures no compiler padding between fields.
// Without it the struct could be padded to 40+ bytes and every
// field after the first would be at the wrong offset.
//
// TODO (Phase 2): dict_size will be non-zero for dictionary-
// encoded columns. The reader uses it to locate the dictionary
// section (starts at offset 36 + data_size).
// ============================================================
#pragma pack(push, 1)
struct ColumnHeader {
    uint32_t magic;      // 4 bytes: must equal MAGIC (0x434F4C31 = 'COL1')
    uint32_t version;    // 4 bytes: must equal VERSION (1)
    uint8_t  type;       // 1 byte:  ColumnType value
    uint8_t  encoding;   // 1 byte:  Encoding value
    uint16_t reserved;   // 2 bytes: always 0
    uint64_t row_count;  // 8 bytes: total rows in column
    uint64_t data_size;  // 8 bytes: size of data section in bytes
    uint64_t dict_size;  // 8 bytes: size of dictionary section (0 = Phase 1)
};
#pragma pack(pop)

using Row = std::vector<std::string>;

// ============================================================
// Constants
// MAGIC = ASCII bytes 'C','O','L','1' = 0x434F4C31
// HEADER_SIZE = sizeof(ColumnHeader) = 36 bytes
// CRC64_SIZE  = 8 bytes (uint64_t appended after all data)
// ============================================================
constexpr uint32_t MAGIC       = 0x434F4C31;
constexpr uint32_t VERSION     = 1;
constexpr size_t   HEADER_SIZE = sizeof(ColumnHeader);  // 36 bytes
constexpr size_t   CRC64_SIZE  = sizeof(uint64_t);      // 8 bytes

// ============================================================
// CRC64 (ECMA-182 polynomial)
// Manual Section 4: "footer_crc64 — CRC64 over everything above"
// meaning over all bytes of the file except the CRC itself.
//
// Uses the standard ECMA-182 polynomial: 0xC96C5795D7870F42
// Table-driven for speed. Implemented from scratch — no library
// allowed per Section 13 ("no existing compression library").
//
// Usage:
//   uint64_t crc = crc64(data_ptr, num_bytes);
//   // or accumulate in chunks:
//   uint64_t crc = 0;
//   crc = crc64_update(crc, chunk1, len1);
//   crc = crc64_update(crc, chunk2, len2);
// ============================================================
uint64_t crc64(const void* data, size_t length);
uint64_t crc64_update(uint64_t crc, const void* data, size_t length);

const char*  typeToString(ColumnType type);
ColumnType   stringToType(const std::string& str);
const char*  encodingToString(Encoding enc);
Encoding     stringToEncoding(const std::string& str);

#endif // COMMON_H
