#ifndef COLUMN_WRITER_H
#define COLUMN_WRITER_H

#include "common.h"
#include <string>
#include <vector>
#include <fstream>
#include <unordered_map>  // Phase 2: for dictionary builder

class ColumnWriter {
public:
    ColumnWriter(const std::string& table_dir,
                 const std::string& column_name,
                 ColumnType type,
                 Encoding encoding);

    ~ColumnWriter();

    void setRowCount(size_t count);

    // Phase 1: raw (no-encoding) write methods
    // INT32: 4-byte signed little-endian per Section 4 (data type 1)
    bool writeInt32(const std::vector<int32_t>& values);
    bool writeInt64(const std::vector<int64_t>& values);
    bool writeDouble(const std::vector<double>& values);
    bool writeString(const std::vector<std::string>& values);

    // Phase 2: dictionary-encoded write for STRING columns.
    // Builds the dictionary from distinct values, writes the id
    // data section, then appends the dictionary at the end.
    // Manual Section 5.1: ids stored as 1, 2, or 4 bytes depending
    // on how many distinct values D there are.
    bool writeDictionary(const std::vector<std::string>& values);

    // Phase 2: RLE write for numeric columns.
    // Scans for runs of identical values and emits (value, length) pairs.
    // Manual Section 5.1: only use RLE if run_count < N/4.
    bool writeRLE_Int32(const std::vector<int32_t>& values);
    bool writeRLE_Int64(const std::vector<int64_t>& values);
    bool writeRLE_Double(const std::vector<double>& values);

    bool finalize();

private:
    std::string filepath_temp_;   // write goes here first
    std::string filepath_final_;  // renamed to this on finalize()

    std::string   column_name_;
    ColumnType    type_;
    Encoding      encoding_;
    std::ofstream file_;
    size_t        row_count_;
    bool          header_written_;   // guard: writeHeader() only writes once
    uint64_t      data_size_written_;

    // Phase 2: dict_size_ is non-zero for DICTIONARY columns.
    // Patched into the header at finalize() just like data_size_.
    uint64_t      dict_size_written_;

    bool openFile();

    // writeHeader() is guarded by header_written_ — safe to call multiple
    // times (e.g. from RLE fallback paths); only the first call writes.
    bool writeHeader();

    void writeInt32Raw(int32_t value);
    void writeInt64Raw(int64_t value);
    void writeDoubleRaw(double value);
    void writeStringRaw(const std::string& value);
    bool closeAndRename();

    // Phase 2: helper — writes a dictionary section entry.
    // Each entry: 2-byte length prefix + bytes (same as STRING raw format).
    void writeDictEntry(const std::string& s);

    // Phase 2: helper — writes an integer id using the minimum byte width
    // that fits D distinct values (1, 2, or 4 bytes).
    void writeDictId(uint32_t id, uint8_t id_width);
};

#endif // COLUMN_WRITER_H