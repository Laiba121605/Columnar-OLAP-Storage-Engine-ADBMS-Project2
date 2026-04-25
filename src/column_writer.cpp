#include "../include/column_writer.h"
#include "../include/common.h"
#include <iostream>
#include <cstdio>
#include <fstream>
#include <vector>

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
{
    // Temp-file-and-rename pattern (Section 5.2):
    // Write to .col.tmp first. On finalize() rename to .col.
    // A crash mid-write never leaves a corrupt .col file.
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
    if (!file_.is_open())
        if (!openFile()) return false;

    // Header layout matches Section 4 exactly (36 bytes total).
    // data_size written as 0 here; corrected in finalize() via seekp.
    // dict_size is always 0 in Phase 1.
    ColumnHeader header;
    header.magic     = MAGIC;
    header.version   = VERSION;
    header.type      = static_cast<uint8_t>(type_);
    header.encoding  = static_cast<uint8_t>(encoding_);
    header.reserved  = 0;
    header.row_count = row_count_;
    header.data_size = 0;   // filled in at finalize()
    header.dict_size = 0;   // always 0 in Phase 1

    file_.write(reinterpret_cast<const char*>(&header), sizeof(ColumnHeader));
    if (file_.fail()) return false;

    header_written_ = true;
    return true;
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
    // Manual Section 4: STRING uses a 2-byte length prefix (uint16_t).
    uint16_t len = static_cast<uint16_t>(value.size());
    file_.write(reinterpret_cast<const char*>(&len), sizeof(uint16_t));
    file_.write(value.c_str(), len);
    data_size_written_ += sizeof(uint16_t) + len;
}

void ColumnWriter::setRowCount(size_t count) { row_count_ = count; }

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

bool ColumnWriter::closeAndRename() {
    if (!file_.is_open()) return true;

    // Step 1: go back and write the real data_size now that we know it.
    // data_size field is at byte offset 20 in the header (Section 4):
    //   4 (magic) + 4 (version) + 1 (type) + 1 (encoding) + 2 (reserved) + 8 (row_count) = 20
    file_.seekp(20, std::ios::beg);
    file_.write(reinterpret_cast<const char*>(&data_size_written_), sizeof(uint64_t));
    if (file_.fail()) return false;

    // Step 2: compute CRC64 over the entire file (header + data).
    // Manual Section 4: "footer_crc64 — CRC64 over everything above"
    // We read back the temp file, compute CRC, then append it.
    //
    // Why read back instead of computing while writing:
    // We needed to seekp back to patch data_size (step 1), which means
    // the header bytes we originally fed to any running CRC accumulator
    // would have the wrong data_size in them. Reading back the final
    // file guarantees we CRC exactly what is on disk.
    file_.flush();
    file_.close();

    // Read the whole temp file into memory to compute CRC
    std::ifstream readback(filepath_temp_, std::ios::binary);
    if (!readback.is_open()) {
        std::cerr << "CRC: cannot reopen temp file: " << filepath_temp_ << "\n";
        return false;
    }
    std::vector<char> buf((std::istreambuf_iterator<char>(readback)),
                           std::istreambuf_iterator<char>());
    readback.close();

    uint64_t crc = crc64(buf.data(), buf.size());

    // Append 8-byte CRC footer
    std::ofstream append(filepath_temp_, std::ios::binary | std::ios::app);
    if (!append.is_open()) {
        std::cerr << "CRC: cannot append to temp file: " << filepath_temp_ << "\n";
        return false;
    }
    append.write(reinterpret_cast<const char*>(&crc), sizeof(uint64_t));
    append.close();

    // Step 3: atomic rename temp -> final
    if (std::rename(filepath_temp_.c_str(), filepath_final_.c_str()) != 0) {
        std::cerr << "Error renaming " << filepath_temp_ << " -> " << filepath_final_ << "\n";
        return false;
    }

    return true;
}

bool ColumnWriter::finalize() {
    return closeAndRename();
}
