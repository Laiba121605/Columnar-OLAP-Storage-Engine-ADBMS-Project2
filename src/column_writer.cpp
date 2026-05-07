#include "../include/column_writer.h"
#include "../include/common.h"
#include <iostream>
#include <cstdio>
#include <fstream>
#include <vector>
#include <unordered_map>
#include <algorithm>

#ifdef _WIN32
    #include <windows.h>
#else
    #include <sys/stat.h>
    #include <unistd.h>
#endif

// Cross-platform directory creation
static void ensure_dir(const std::string& dir) {
#ifdef _WIN32
    std::string cmd = "mkdir \"" + dir + "\" 2> nul";
#else
    std::string cmd = "mkdir -p \"" + dir + "\"";
#endif
    system(cmd.c_str());
}

ColumnWriter::ColumnWriter(const std::string& table_dir,
                           const std::string& column_name,
                           ColumnType type,
                           Encoding encoding)
    : column_name_(column_name)
    , type_(type)
    , encoding_(encoding)
    , row_count_(0)
    , header_written_(false)
    , data_size_written_(0)
    , dict_size_written_(0)   // Phase 2: initialise dict size tracker
{
    filepath_temp_  = table_dir + "/" + column_name + ".col.tmp";
    filepath_final_ = table_dir + "/" + column_name + ".col";
}

ColumnWriter::~ColumnWriter() {
    if (file_.is_open()) file_.close();
}

bool ColumnWriter::openFile() {
    size_t slash = filepath_temp_.find_last_of("/\\");
    if (slash != std::string::npos)
        ensure_dir(filepath_temp_.substr(0, slash));

    file_.open(filepath_temp_, std::ios::binary);
    return file_.is_open();
}

bool ColumnWriter::writeHeader() {
    if (header_written_) return true;   // guard: never write header twice

    if (!file_.is_open())
        if (!openFile()) return false;

    // Header layout matches Section 4 exactly (36 bytes total).
    // data_size and dict_size are written as 0 here and patched
    // in closeAndRename() once we know the real sizes.
    ColumnHeader header;
    header.magic     = MAGIC;
    header.version   = VERSION;
    header.type      = static_cast<uint8_t>(type_);
    header.encoding  = static_cast<uint8_t>(encoding_);
    header.reserved  = 0;
    header.row_count = row_count_;
    header.data_size = 0;   // patched in closeAndRename()
    header.dict_size = 0;   // patched in closeAndRename()

    file_.write(reinterpret_cast<const char*>(&header), sizeof(ColumnHeader));
    if (file_.fail()) return false;

    header_written_ = true;
    return true;
}

// ── Raw write helpers (Phase 1, still used in Phase 2 for non-encoded cols) ──

void ColumnWriter::writeInt32Raw(int32_t value) {
    file_.write(reinterpret_cast<const char*>(&value), sizeof(int32_t));
    data_size_written_ += sizeof(int32_t);
}

void ColumnWriter::writeInt64Raw(int64_t value) {
    file_.write(reinterpret_cast<const char*>(&value), sizeof(int64_t));
    data_size_written_ += sizeof(int64_t);
}

void ColumnWriter::writeDoubleRaw(double value) {
    file_.write(reinterpret_cast<const char*>(&value), sizeof(double));
    data_size_written_ += sizeof(double);
}

void ColumnWriter::writeStringRaw(const std::string& value) {
    uint16_t len = static_cast<uint16_t>(value.size());
    file_.write(reinterpret_cast<const char*>(&len), sizeof(uint16_t));
    file_.write(value.c_str(), len);
    data_size_written_ += sizeof(uint16_t) + len;
}

void ColumnWriter::setRowCount(size_t count) { row_count_ = count; }

// ── Phase 1 public write methods ─────────────────────────────────────────────

bool ColumnWriter::writeInt32(const std::vector<int32_t>& values) {
    if (!writeHeader()) return false;
    for (int32_t v : values) { writeInt32Raw(v); if (file_.fail()) return false; }
    return true;
}

bool ColumnWriter::writeInt64(const std::vector<int64_t>& values) {
    if (!writeHeader()) return false;
    for (int64_t v : values) { writeInt64Raw(v); if (file_.fail()) return false; }
    return true;
}

bool ColumnWriter::writeDouble(const std::vector<double>& values) {
    if (!writeHeader()) return false;
    for (double v : values) { writeDoubleRaw(v); if (file_.fail()) return false; }
    return true;
}

bool ColumnWriter::writeString(const std::vector<std::string>& values) {
    if (!writeHeader()) return false;
    for (const auto& v : values) { writeStringRaw(v); if (file_.fail()) return false; }
    return true;
}

// ── Phase 2: Dictionary helpers ───────────────────────────────────────────────

// writeDictEntry: appends one string to the dictionary section.
// Format: 2-byte length prefix (uint16_t) + raw bytes.
// Same on-disk format as writeStringRaw so the reader can reuse
// the same decode logic for both raw strings and dictionary entries.
void ColumnWriter::writeDictEntry(const std::string& s) {
    uint16_t len = static_cast<uint16_t>(s.size());
    file_.write(reinterpret_cast<const char*>(&len), sizeof(uint16_t));
    file_.write(s.c_str(), len);
    dict_size_written_ += sizeof(uint16_t) + len;
}

// writeDictId: writes a dictionary integer id using the minimum
// byte width that fits D distinct values.
//   id_width == 1 → uint8_t   (D <= 256)
//   id_width == 2 → uint16_t  (D <= 65536)
//   id_width == 4 → uint32_t  (D > 65536, rare)
// Manual Section 5.1: "Pick the smallest that fits."
void ColumnWriter::writeDictId(uint32_t id, uint8_t id_width) {
    if (id_width == 1) {
        uint8_t v = static_cast<uint8_t>(id);
        file_.write(reinterpret_cast<const char*>(&v), 1);
        data_size_written_ += 1;
    } else if (id_width == 2) {
        uint16_t v = static_cast<uint16_t>(id);
        file_.write(reinterpret_cast<const char*>(&v), 2);
        data_size_written_ += 2;
    } else {
        file_.write(reinterpret_cast<const char*>(&id), 4);
        data_size_written_ += 4;
    }
}

// ── Phase 2: Dictionary-encoded writer ───────────────────────────────────────
//
// writeDictionary()
// Manual Section 5.1: STRING columns use dictionary encoding.
// Steps:
//   1. First pass: collect all distinct strings and sort them so the
//      dictionary order is deterministic (makes debugging easier).
//   2. Assign each string an integer id (0 .. D-1).
//   3. Choose id_width = 1/2/4 bytes to fit D.
//   4. Write header (encoding = DICTIONARY).
//   5. Data section: for each row, write its id using id_width bytes.
//   6. Dictionary section: for each entry in order, write as
//      2-byte length prefix + raw bytes.
//   7. dict_size_ is patched into the header in closeAndRename().
//
// If D > 65535, fall back to raw string storage (no encoding).
// Manual Section 5.1: "If the column has more than 65,535 distinct
// values, fall back to no encoding."
// ─────────────────────────────────────────────────────────────────────────────
bool ColumnWriter::writeDictionary(const std::vector<std::string>& values) {
    // ── Step 1: count distinct values ──
    std::unordered_map<std::string, uint32_t> str_to_id;
    std::vector<std::string> id_to_str;  // index = id, value = string

    for (const auto& v : values) {
        if (str_to_id.find(v) == str_to_id.end()) {
            uint32_t id = static_cast<uint32_t>(id_to_str.size());
            str_to_id[v] = id;
            id_to_str.push_back(v);
        }
    }

    uint32_t D = static_cast<uint32_t>(id_to_str.size());

    // ── Fallback: too many distinct values → raw string storage ──
    if (D > 65535) {
        // Override encoding to NONE and write raw
        encoding_ = Encoding::NONE;
        return writeString(values);
    }

    // ── Step 2: choose id width ──
    uint8_t id_width;
    if      (D <= 256)   id_width = 1;
    else if (D <= 65536) id_width = 2;
    else                  id_width = 4;

    // ── Step 3: write header (encoding = DICTIONARY) ──
    if (!writeHeader()) return false;

    // ── Step 4: write data section (ids) ──
    for (const auto& v : values) {
        writeDictId(str_to_id[v], id_width);
        if (file_.fail()) return false;
    }

    // ── Step 5: write dictionary section ──
    // Format: for each entry (in id order): uint16_t length + bytes.
    // The reader seeks to (HEADER_SIZE + data_size_) to find this section.
    dict_size_written_ = 0;
    for (const auto& s : id_to_str) {
        writeDictEntry(s);
        if (file_.fail()) return false;
    }

    return true;
}

// ── Phase 2: RLE encoder for INT64 ───────────────────────────────────────────
//
// writeRLE_Int64()
// Manual Section 5.1: RLE data section = sequence of (value, run_length) pairs.
// Each pair: 8 bytes (int64_t value) + 4 bytes (uint32_t run_length).
// Only use RLE if run_count < N/4 — otherwise it costs more than raw storage.
// ─────────────────────────────────────────────────────────────────────────────
bool ColumnWriter::writeRLE_Int32(const std::vector<int32_t>& values) {
    // Convert to int64 and delegate — RLE pairs always store 8-byte values
    // so both INT32 and INT64 columns use the same on-disk pair format.
    std::vector<int64_t> vals64(values.begin(), values.end());
    return writeRLE_Int64(vals64);
}

bool ColumnWriter::writeRLE_Int64(const std::vector<int64_t>& values) {
    if (values.empty()) {
        return writeInt64(values);  // fall back for empty column
    }

    // ── First pass: count runs ──
    size_t run_count = 1;
    for (size_t i = 1; i < values.size(); i++) {
        if (values[i] != values[i - 1]) run_count++;
    }

    // ── Decision: only use RLE if run_count < N/4 ──
    // Manual Section 5.1: "the loader should only choose RLE if the
    // number of runs is less than about N/4"
    if (run_count >= values.size() / 4) {
        // Not worth it — fall back to raw storage
        encoding_ = Encoding::NONE;
        return writeInt64(values);
    }

    // ── Write header (encoding = RLE) ──
    if (!writeHeader()) return false;

    // ── Write (value, run_length) pairs ──
    size_t i = 0;
    while (i < values.size()) {
        int64_t  val = values[i];
        uint64_t run = 1;
        while (i + run < values.size() && values[i + run] == val) {
            run++;
        }
        // Write value (8 bytes) then run_length (8 bytes, uint64_t)
        file_.write(reinterpret_cast<const char*>(&val), sizeof(int64_t));
        file_.write(reinterpret_cast<const char*>(&run), sizeof(uint64_t));
        data_size_written_ += sizeof(int64_t) + sizeof(uint64_t);
        if (file_.fail()) return false;
        i += run;
    }

    return true;
}

// ── Phase 2: RLE encoder for DOUBLE ──────────────────────────────────────────
//
// writeRLE_Double()
// Same structure as writeRLE_Int64 but uses double values.
// Each pair: 8 bytes (double) + 4 bytes (uint32_t run_length).
// Note: floating-point equality comparison is fine here because we are
// comparing values that came from the same CSV string parse — two rows
// with the same text will produce the exact same IEEE 754 bit pattern.
// ─────────────────────────────────────────────────────────────────────────────
bool ColumnWriter::writeRLE_Double(const std::vector<double>& values) {
    if (values.empty()) {
        return writeDouble(values);
    }

    // First pass: count runs
    size_t run_count = 1;
    for (size_t i = 1; i < values.size(); i++) {
        if (values[i] != values[i - 1]) run_count++;
    }

    if (run_count >= values.size() / 4) {
        encoding_ = Encoding::NONE;
        return writeDouble(values);
    }

    if (!writeHeader()) return false;

    size_t i = 0;
    while (i < values.size()) {
        double   val = values[i];
        uint64_t run = 1;
        while (i + run < values.size() && values[i + run] == val) {
            run++;
        }
        file_.write(reinterpret_cast<const char*>(&val), sizeof(double));
        file_.write(reinterpret_cast<const char*>(&run), sizeof(uint64_t));
        data_size_written_ += sizeof(double) + sizeof(uint64_t);
        if (file_.fail()) return false;
        i += run;
    }

    return true;
}

// ── closeAndRename ────────────────────────────────────────────────────────────
//
// Patches the header with real data_size and dict_size, computes CRC64
// over the whole file, appends it as an 8-byte footer, then atomically
// renames .col.tmp -> .col.
// ─────────────────────────────────────────────────────────────────────────────
bool ColumnWriter::closeAndRename() {
    if (!file_.is_open()) return true;

    // Patch data_size at byte offset 20
    file_.seekp(20, std::ios::beg);
    file_.write(reinterpret_cast<const char*>(&data_size_written_), sizeof(uint64_t));
    if (file_.fail()) return false;

    // Phase 2: patch dict_size at byte offset 28
    file_.seekp(28, std::ios::beg);
    file_.write(reinterpret_cast<const char*>(&dict_size_written_), sizeof(uint64_t));
    if (file_.fail()) return false;

    // Compute CRC64 over entire file, append as footer
    file_.flush();
    file_.close();

    std::ifstream readback(filepath_temp_, std::ios::binary);
    if (!readback.is_open()) {
        std::cerr << "CRC: cannot reopen temp file: " << filepath_temp_ << "\n";
        return false;
    }
    std::vector<char> buf((std::istreambuf_iterator<char>(readback)),
                           std::istreambuf_iterator<char>());
    readback.close();

    uint64_t crc = crc64(buf.data(), buf.size());

    std::ofstream append(filepath_temp_, std::ios::binary | std::ios::app);
    if (!append.is_open()) {
        std::cerr << "CRC: cannot append to temp file: " << filepath_temp_ << "\n";
        return false;
    }
    append.write(reinterpret_cast<const char*>(&crc), sizeof(uint64_t));
    append.close();

    // Atomic rename temp -> final
    // Phase 2 note: same Windows fix as schema.cpp — remove destination first
#ifdef _WIN32
    std::remove(filepath_final_.c_str());
#endif
    if (std::rename(filepath_temp_.c_str(), filepath_final_.c_str()) != 0) {
        std::cerr << "Error renaming " << filepath_temp_
                  << " -> " << filepath_final_ << "\n";
        return false;
    }

    return true;
}

bool ColumnWriter::finalize() {
    return closeAndRename();
}