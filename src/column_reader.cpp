#include "../include/column_reader.h"
#include <iostream>
#include <cstring>

// ============================================================
// ColumnReader implementation — P2: Storage Read
// ============================================================

ColumnReader::ColumnReader()
    : type_(ColumnType::INT64)
    , encoding_(Encoding::NONE)
    , row_count_(0)
    , data_size_(0)
    , dict_size_(0)
    , rows_read_(0)
    , is_open_(false)
{}

ColumnReader::~ColumnReader() {
    close();
}

// ============================================================
// open()
// Steps:
//   1. Build the filepath from table_dir + column_name + ".col"
//   2. Open the file in binary mode
//   3. Verify CRC64 footer (catches corruption)
//   4. Read and validate the 36-byte header
//   5. Seek to start of data section (offset 36)
//   6. TODO Phase 2: if DICTIONARY, load dictionary into memory
// ============================================================
bool ColumnReader::open(const std::string& table_dir,
                        const std::string& column_name) {
    column_name_ = column_name;
    filepath_    = table_dir + "/" + column_name + ".col";

    file_.open(filepath_, std::ios::binary);
    if (!file_.is_open()) {
        last_error_ = "Cannot open file: " + filepath_;
        return false;
    }

    // Step 1: verify CRC before reading anything.
    // Manual Section 4: footer_crc64 covers everything above it.
    // If the file is corrupt we refuse to read it.
    if (!verifyCRC()) {
        file_.close();
        return false;
    }

    // Step 2: read and validate header.
    if (!verifyAndLoadHeader()) {
        file_.close();
        return false;
    }

    // Step 3: record where data starts so reset() can return here.
    // Data always starts at byte 36 (right after the 36-byte header).
    data_start_ = HEADER_SIZE;
    file_.seekg(data_start_);

    // TODO Phase 2: if encoding_ == DICTIONARY, call loadDictionary()
    // here. The dictionary sits after the data section at offset
    // (HEADER_SIZE + data_size_), and is dict_size_ bytes long.

    is_open_   = true;
    rows_read_ = 0;
    return true;
}

// ============================================================
// verifyCRC()
// Reads the entire file, computes CRC64 over everything except
// the last 8 bytes, and compares with the stored footer.
// Manual Section 4: "footer_crc64 — CRC64 over everything above"
// ============================================================
bool ColumnReader::verifyCRC() {
    // Read entire file
    file_.seekg(0, std::ios::end);
    std::streamsize file_size = file_.tellg();
    file_.seekg(0, std::ios::beg);

    if (file_size < static_cast<std::streamsize>(HEADER_SIZE + CRC64_SIZE)) {
        last_error_ = "File too small to be valid: " + filepath_;
        return false;
    }

    std::vector<char> buf(file_size);
    file_.read(buf.data(), file_size);
    if (file_.fail()) {
        last_error_ = "Failed to read file for CRC check: " + filepath_;
        return false;
    }

    // Last 8 bytes = stored CRC
    uint64_t stored_crc = 0;
    memcpy(&stored_crc, buf.data() + file_size - CRC64_SIZE, CRC64_SIZE);

    // Compute CRC over everything before the footer
    uint64_t computed_crc = crc64(buf.data(), file_size - CRC64_SIZE);

    if (stored_crc != computed_crc) {
        last_error_ = "CRC mismatch — file may be corrupt: " + filepath_;
        return false;
    }

    // Seek back to beginning for header read
    file_.seekg(0, std::ios::beg);
    return true;
}

// ============================================================
// verifyAndLoadHeader()
// Reads the 36-byte header and validates every field.
// Populates type_, encoding_, row_count_, data_size_, dict_size_.
// ============================================================
bool ColumnReader::verifyAndLoadHeader() {
    ColumnHeader header;
    file_.read(reinterpret_cast<char*>(&header), sizeof(ColumnHeader));
    if (file_.fail()) {
        last_error_ = "Failed to read header: " + filepath_;
        return false;
    }

    // Validate magic bytes
    if (header.magic != MAGIC) {
        last_error_ = "Invalid magic bytes in: " + filepath_;
        return false;
    }

    // Validate version
    if (header.version != VERSION) {
        last_error_ = "Unsupported version " +
                      std::to_string(header.version) + " in: " + filepath_;
        return false;
    }

    // Validate reserved field
    if (header.reserved != 0) {
        last_error_ = "Reserved field not zero in: " + filepath_;
        return false;
    }

    // Load metadata
    type_      = static_cast<ColumnType>(header.type);
    encoding_  = static_cast<Encoding>(header.encoding);
    row_count_ = header.row_count;
    data_size_ = header.data_size;
    dict_size_ = header.dict_size;

    return true;
}

// ============================================================
// hasNext()
// True if there are still rows left to read.
// ============================================================
bool ColumnReader::hasNext() const {
    return is_open_ && (rows_read_ < row_count_);
}

// ============================================================
// next()
// Returns the next decoded value and advances the cursor.
// Dispatches to the right decoder based on encoding_.
// ============================================================
ColumnValue ColumnReader::next() {
    rows_read_++;

    switch (encoding_) {
        case Encoding::NONE:
            return readNextNone();

        case Encoding::DICTIONARY:
            // TODO Phase 2: return readNextDict();
            last_error_ = "DICTIONARY encoding not yet implemented (Phase 2)";
            return std::string("");

        case Encoding::RLE:
            // TODO Phase 2: return readNextRLE();
            last_error_ = "RLE encoding not yet implemented (Phase 2)";
            return int64_t(0);

        default:
            last_error_ = "Unknown encoding";
            return int64_t(0);
    }
}

// ============================================================
// readNextNone()
// Raw decoding — read the value directly from the file.
// Dispatches on column type (INT64, DOUBLE, STRING).
// Manual Section 6.1: "No encoding: read the data section as-is,
// one value at a time."
// ============================================================
ColumnValue ColumnReader::readNextNone() {
    switch (type_) {

        case ColumnType::INT64: {
            int64_t val = 0;
            file_.read(reinterpret_cast<char*>(&val), sizeof(int64_t));
            return val;
        }

        case ColumnType::INT32: {
            // TODO Phase 2/3: INT32 used for date columns
            int32_t val = 0;
            file_.read(reinterpret_cast<char*>(&val), sizeof(int32_t));
            return static_cast<int64_t>(val);  // promote to int64 for uniformity
        }

        case ColumnType::DOUBLE: {
            double val = 0.0;
            file_.read(reinterpret_cast<char*>(&val), sizeof(double));
            return val;
        }

        case ColumnType::STRING: {
            // Manual Section 4: STRING = 2-byte length prefix + bytes
            // Must match exactly what P1's writeStringRaw() wrote.
            uint16_t len = 0;
            file_.read(reinterpret_cast<char*>(&len), sizeof(uint16_t));
            std::string val(len, '\0');
            file_.read(&val[0], len);
            return val;
        }

        default:
            last_error_ = "Unknown column type";
            return int64_t(0);
    }
}

// ============================================================
// TODO Phase 2: readNextDict()
// Load dictionary on open(), then for each row:
//   - read a small integer id (1, 2, or 4 bytes depending on D)
//   - return dictionary_[id]
// Manual Section 6.1: "read the dictionary into memory (it is
// small). Then read the data section as small integers; for each
// integer, look up the corresponding string in the dictionary."
// ============================================================
ColumnValue ColumnReader::readNextDict() {
    // Placeholder — implement in Phase 2
    return std::string("");
}

// ============================================================
// TODO Phase 2: readNextRLE()
// Keep track of current (value, remaining_count) pair.
// When remaining_count hits 0, read the next pair from file.
// Manual Section 6.1: "read the data section as (value, length)
// pairs. The iterator emits value length times before advancing."
// ============================================================
ColumnValue ColumnReader::readNextRLE() {
    // Placeholder — implement in Phase 2
    return int64_t(0);
}

// ============================================================
// TODO Phase 2: loadDictionary()
// Called once on open() when encoding == DICTIONARY.
// Seeks to (HEADER_SIZE + data_size_), reads dict_size_ bytes,
// parses the string entries and fills dictionary_ vector.
// ============================================================
void ColumnReader::loadDictionary() {
    // Placeholder — implement in Phase 2
}

// ============================================================
// reset()
// Seeks back to the start of the data section.
// The executor uses this when it needs to scan a column twice —
// first to build a predicate bitmap, then to aggregate.
// Manual Section 6.2: bitmap approach requires reading predicate
// column first, then scanning aggregate columns.
// ============================================================
void ColumnReader::reset() {
    if (!is_open_) return;
    file_.seekg(data_start_);
    rows_read_ = 0;
}

// ============================================================
// close()
// ============================================================
void ColumnReader::close() {
    if (file_.is_open()) file_.close();
    is_open_   = false;
    rows_read_ = 0;
}
