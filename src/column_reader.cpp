#include "../include/column_reader.h"
#include <iostream>
#include <cstring>

// ============================================================
// ColumnReader implementation
// Phase 1: NONE encoding
// Phase 2: DICTIONARY and RLE decoders
// ============================================================

ColumnReader::ColumnReader()
    : type_(ColumnType::INT64)
    , encoding_(Encoding::NONE)
    , row_count_(0)
    , data_size_(0)
    , dict_size_(0)
    , rows_read_(0)
    , is_open_(false)
    , id_width_(1)           // Phase 2: default, overwritten in loadDictionary()
    , rle_remaining_(0)      // Phase 2: no run loaded yet
    , rle_run_loaded_(false) // Phase 2: first call to readNextRLE will load first pair
{}

ColumnReader::~ColumnReader() {
    close();
}

// ============================================================
// open()
// Steps:
//   1. Open file in binary mode
//   2. Verify CRC64 footer
//   3. Read and validate 36-byte header
//   4. Seek to start of data section (offset 36)
//   5. Phase 2: if DICTIONARY, load dictionary into memory
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

    if (!verifyCRC()) {
        file_.close();
        return false;
    }

    if (!verifyAndLoadHeader()) {
        file_.close();
        return false;
    }

    data_start_ = HEADER_SIZE;
    file_.seekg(data_start_);

    // Phase 2: load dictionary into memory if this is a DICTIONARY column.
    // The dictionary sits after the data section and is dict_size_ bytes long.
    // We must do this before any calls to next() so the id->string map is ready.
    if (encoding_ == Encoding::DICTIONARY) {
        loadDictionary();
        // Seek back to data section after loading dictionary
        file_.seekg(data_start_);
    }

    // Phase 2: initialise RLE state so readNextRLE() knows it must
    // load the first (value, run_length) pair on its first call.
    if (encoding_ == Encoding::RLE) {
        rle_remaining_  = 0;
        rle_run_loaded_ = false;
    }

    is_open_   = true;
    rows_read_ = 0;
    return true;
}

// ============================================================
// verifyCRC()
// ============================================================
bool ColumnReader::verifyCRC() {
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

    uint64_t stored_crc = 0;
    memcpy(&stored_crc, buf.data() + file_size - CRC64_SIZE, CRC64_SIZE);
    uint64_t computed_crc = crc64(buf.data(), file_size - CRC64_SIZE);

    if (stored_crc != computed_crc) {
        last_error_ = "CRC mismatch — file may be corrupt: " + filepath_;
        return false;
    }

    file_.seekg(0, std::ios::beg);
    return true;
}

// ============================================================
// verifyAndLoadHeader()
// ============================================================
bool ColumnReader::verifyAndLoadHeader() {
    ColumnHeader header;
    file_.read(reinterpret_cast<char*>(&header), sizeof(ColumnHeader));
    if (file_.fail()) {
        last_error_ = "Failed to read header: " + filepath_;
        return false;
    }

    if (header.magic != MAGIC) {
        last_error_ = "Invalid magic bytes in: " + filepath_;
        return false;
    }
    if (header.version != VERSION) {
        last_error_ = "Unsupported version " +
                      std::to_string(header.version) + " in: " + filepath_;
        return false;
    }
    if (header.reserved != 0) {
        last_error_ = "Reserved field not zero in: " + filepath_;
        return false;
    }

    type_      = static_cast<ColumnType>(header.type);
    encoding_  = static_cast<Encoding>(header.encoding);
    row_count_ = header.row_count;
    data_size_ = header.data_size;
    dict_size_ = header.dict_size;

    return true;
}

// ============================================================
// hasNext()
// ============================================================
bool ColumnReader::hasNext() const {
    return is_open_ && (rows_read_ < row_count_);
}

// ============================================================
// next()
// Dispatches to the right decoder based on encoding_.
// ============================================================
ColumnValue ColumnReader::next() {
    rows_read_++;
    switch (encoding_) {
        case Encoding::NONE:       return readNextNone();
        case Encoding::DICTIONARY: return readNextDict();
        case Encoding::RLE:        return readNextRLE();
        default:
            last_error_ = "Unknown encoding";
            return int64_t(0);
    }
}

// ============================================================
// readNextNone() — Phase 1, unchanged
// ============================================================
ColumnValue ColumnReader::readNextNone() {
    switch (type_) {
        case ColumnType::INT64: {
            int64_t val = 0;
            file_.read(reinterpret_cast<char*>(&val), sizeof(int64_t));
            return val;
        }
        case ColumnType::INT32: {
            int32_t val = 0;
            file_.read(reinterpret_cast<char*>(&val), sizeof(int32_t));
            return static_cast<int64_t>(val);
        }
        case ColumnType::DOUBLE: {
            double val = 0.0;
            file_.read(reinterpret_cast<char*>(&val), sizeof(double));
            return val;
        }
        case ColumnType::STRING: {
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
// Phase 2: loadDictionary()
// Called once in open() when encoding == DICTIONARY.
// Seeks to (HEADER_SIZE + data_size_) — the byte right after
// the data section — and reads dict_size_ bytes, parsing
// each entry as: uint16_t length + raw bytes.
// Fills dictionary_[] so readNextDict() can do O(1) lookups.
//
// Also determines id_width_ from the number of entries D:
//   D <= 256   → id_width_ = 1
//   D <= 65536 → id_width_ = 2
//   else       → id_width_ = 4
// This must match exactly what writeDictId() used when writing.
// ============================================================
void ColumnReader::loadDictionary() {
    dictionary_.clear();

    // Seek to the dictionary section: right after the data section.
    std::streampos dict_start = HEADER_SIZE + static_cast<std::streampos>(data_size_);
    file_.seekg(dict_start);

    std::streampos dict_end = dict_start + static_cast<std::streampos>(dict_size_);

    while (file_.tellg() < dict_end && !file_.fail()) {
        uint16_t len = 0;
        file_.read(reinterpret_cast<char*>(&len), sizeof(uint16_t));
        if (file_.fail()) break;

        std::string s(len, '\0');
        file_.read(&s[0], len);
        if (file_.fail()) break;

        dictionary_.push_back(s);
    }

    // Derive id_width_ from D, matching what the writer used
    uint32_t D = static_cast<uint32_t>(dictionary_.size());
    if      (D <= 256)   id_width_ = 1;
    else if (D <= 65536) id_width_ = 2;
    else                  id_width_ = 4;
}

// ============================================================
// Phase 2: readNextDict()
// Reads one id from the data section (id_width_ bytes),
// looks it up in dictionary_[], returns the string.
// Manual Section 6.1: "read the data section as small integers;
// for each integer, look up the corresponding string in the
// dictionary."
// ============================================================
ColumnValue ColumnReader::readNextDict() {
    uint32_t id = 0;

    if (id_width_ == 1) {
        uint8_t v = 0;
        file_.read(reinterpret_cast<char*>(&v), 1);
        id = v;
    } else if (id_width_ == 2) {
        uint16_t v = 0;
        file_.read(reinterpret_cast<char*>(&v), 2);
        id = v;
    } else {
        file_.read(reinterpret_cast<char*>(&id), 4);
    }

    if (file_.fail() || id >= dictionary_.size()) {
        last_error_ = "Dictionary id out of range or read error";
        return std::string("");
    }

    return dictionary_[id];
}

// ============================================================
// Phase 2: readNextRLE()
// Maintains (rle_current_value_, rle_remaining_) state across calls.
// When rle_remaining_ hits 0, reads the next (value, run_length)
// pair from the file.
// Manual Section 6.1: "The iterator emits value length times
// before advancing to the next pair."
//
// On-disk format per pair:
//   INT64 column:  int64_t (8 bytes) + uint64_t run_length (8 bytes)
//   DOUBLE column: double  (8 bytes) + uint64_t run_length (8 bytes)
// ============================================================
ColumnValue ColumnReader::readNextRLE() {
    // Load next pair if no rows remain in the current run
    if (rle_remaining_ == 0) {
        // run_length is stored as uint64_t on disk.
        // The writer (writeRLEColumn test helper and column_writer.cpp) both
        // use uint64_t for run_length.  The original code used uint32_t which
        // caused it to read only 4 of the 8 bytes, leaving the file pointer
        // misaligned for every subsequent pair.
        uint64_t run = 0;

        if (type_ == ColumnType::INT64 || type_ == ColumnType::INT32) {
            int64_t val = 0;
            file_.read(reinterpret_cast<char*>(&val), sizeof(int64_t));
            file_.read(reinterpret_cast<char*>(&run), sizeof(uint64_t));
            if (file_.fail() || run == 0) {
                last_error_ = "RLE read error or zero-length run";
                return int64_t(0);
            }
            rle_current_value_ = val;
        } else if (type_ == ColumnType::DOUBLE) {
            double val = 0.0;
            file_.read(reinterpret_cast<char*>(&val), sizeof(double));
            file_.read(reinterpret_cast<char*>(&run), sizeof(uint64_t));
            if (file_.fail() || run == 0) {
                last_error_ = "RLE read error or zero-length run";
                return double(0.0);
            }
            rle_current_value_ = val;
        } else {
            last_error_ = "RLE not supported for STRING columns";
            return int64_t(0);
        }

        rle_remaining_  = run;
        rle_run_loaded_ = true;
    }

    rle_remaining_--;
    return rle_current_value_;
}

// ============================================================
// reset()
// Seeks back to data section start.
// Phase 2: also resets RLE state so the decoder starts fresh.
// DICTIONARY doesn't need reset — dictionary_[] stays in memory.
// ============================================================
void ColumnReader::reset() {
    if (!is_open_) return;
    file_.seekg(data_start_);
    rows_read_ = 0;

    // Phase 2: reset RLE state so next call reads a fresh pair
    if (encoding_ == Encoding::RLE) {
        rle_remaining_  = 0;
        rle_run_loaded_ = false;
    }
}

// ============================================================
// close()
// ============================================================
void ColumnReader::close() {
    if (file_.is_open()) file_.close();
    is_open_   = false;
    rows_read_ = 0;
    dictionary_.clear();
    rle_remaining_  = 0;
    rle_run_loaded_ = false;
}