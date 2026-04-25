#include "../include/common.h"

// ============================================================
// CRC64 lookup table (ECMA-182 polynomial: 0xC96C5795D7870F42)
// Generated once at startup via the standard reflected algorithm.
// Each entry is the CRC of a single byte value (0-255).
// The table makes per-byte CRC computation a single XOR + lookup
// instead of 8 bit-by-bit iterations.
// ============================================================
static const uint64_t CRC64_POLY = 0xC96C5795D7870F42ULL;

static uint64_t crc64_table[256];
static bool     crc64_table_ready = false;

static void build_crc64_table() {
    for (int i = 0; i < 256; i++) {
        uint64_t crc = static_cast<uint64_t>(i);
        for (int bit = 0; bit < 8; bit++) {
            if (crc & 1)
                crc = (crc >> 1) ^ CRC64_POLY;
            else
                crc >>= 1;
        }
        crc64_table[i] = crc;
    }
    crc64_table_ready = true;
}

// Update a running CRC with more data.
// Initialize crc to 0 before the first call.
// The final value is the CRC of all data fed in.
uint64_t crc64_update(uint64_t crc, const void* data, size_t length) {
    if (!crc64_table_ready) build_crc64_table();

    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(data);
    for (size_t i = 0; i < length; i++) {
        uint8_t index = static_cast<uint8_t>(crc ^ bytes[i]);
        crc = crc64_table[index] ^ (crc >> 8);
    }
    return crc;
}

// Compute CRC64 of a single contiguous buffer.
uint64_t crc64(const void* data, size_t length) {
    return crc64_update(0ULL, data, length);
}

// ============================================================
// Type/Encoding string converters
// ============================================================
const char* typeToString(ColumnType type) {
    switch (type) {
        case ColumnType::INT32:  return "int32";
        case ColumnType::INT64:  return "int64";
        case ColumnType::DOUBLE: return "double";
        case ColumnType::STRING: return "string";
        default:                 return "unknown";
    }
}

ColumnType stringToType(const std::string& str) {
    if (str == "int32")  return ColumnType::INT32;
    if (str == "int64")  return ColumnType::INT64;
    if (str == "double") return ColumnType::DOUBLE;
    return ColumnType::STRING;
}

const char* encodingToString(Encoding enc) {
    switch (enc) {
        case Encoding::NONE:       return "none";
        case Encoding::DICTIONARY: return "dictionary";
        case Encoding::RLE:        return "rle";
        default:                   return "unknown";
    }
}

// stringToEncoding: parses the encoding field back from schema.json.
// Critical in Phase 2 when DICTIONARY and RLE columns exist — the
// reader needs to know which decoder to use.
Encoding stringToEncoding(const std::string& str) {
    if (str == "dictionary") return Encoding::DICTIONARY;
    if (str == "rle")        return Encoding::RLE;
    return Encoding::NONE;
}
