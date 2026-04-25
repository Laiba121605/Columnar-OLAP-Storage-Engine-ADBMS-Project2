#ifndef COLUMN_READER_H
#define COLUMN_READER_H

// ============================================================
// column_reader.h — P2: Storage Read (Bridge Guy)
// ============================================================
// This is the bridge between P1's storage files and P3's query
// engine. P3 calls open() then next() repeatedly to get values.
// P3 never touches a .col file directly — everything goes through
// this interface.
//
// Manual Section 6.1: "The iterator abstraction is what makes
// everything downstream uniform. The filter and the aggregator
// do not care how the column was encoded; they just pull values
// one at a time."
//
// Phase 1: NONE encoding only (raw values)
// TODO Phase 2: add DICTIONARY and RLE decoders
// ============================================================

#include "common.h"
#include <string>
#include <vector>
#include <fstream>
#include <variant>

// ColumnValue: a single decoded value from any column type.
// P3 receives this and checks which type it is.
// std::variant holds exactly one of: int64, double, or string.
using ColumnValue = std::variant<int64_t, double, std::string>;

// ============================================================
// ColumnReader
// One instance per column being read. P3 creates one for each
// column it needs, reads through them in lockstep (row by row),
// and closes them when done.
//
// Usage:
//   ColumnReader r;
//   r.open("warehouse/sales", "price");
//   while (r.hasNext()) {
//       ColumnValue v = r.next();
//       double price = std::get<double>(v);
//   }
// ============================================================
class ColumnReader {
public:
    ColumnReader();
    ~ColumnReader();

    // Open a column file for reading.
    // table_dir: path to the table directory e.g. "warehouse/sales"
    // column_name: name of the column e.g. "price"
    // Returns false if file not found, CRC invalid, or header corrupt.
    bool open(const std::string& table_dir, const std::string& column_name);

    // Returns true if there are more rows to read.
    bool hasNext() const;

    // Returns the next decoded value and advances the cursor.
    // Call hasNext() first — calling next() past the end is undefined.
    ColumnValue next();

    // Reset cursor back to the first row.
    // Used by the executor when it needs to scan a column twice
    // (e.g. once for the predicate bitmap, once for the aggregate).
    void reset();

    // Close the file and free resources.
    void close();

    // Getters so P3 can inspect the column's metadata.
    ColumnType  getType()     const { return type_; }
    Encoding    getEncoding() const { return encoding_; }
    uint64_t    getRowCount() const { return row_count_; }
    std::string getColumnName() const { return column_name_; }

    // Returns true if the file was successfully opened and CRC verified.
    bool isOpen() const { return is_open_; }

    // Returns the last error message (empty if no error).
    std::string getLastError() const { return last_error_; }

private:
    std::string  column_name_;
    std::string  filepath_;
    std::ifstream file_;
    ColumnType   type_;
    Encoding     encoding_;
    uint64_t     row_count_;
    uint64_t     data_size_;
    uint64_t     dict_size_;
    uint64_t     rows_read_;
    bool         is_open_;
    std::string  last_error_;

    // Offset in the file where the data section begins (= HEADER_SIZE = 36)
    // Stored so reset() can seekg back here without recalculating.
    std::streampos data_start_;

    // Dictionary (loaded into memory on open for DICTIONARY encoding)
    // TODO Phase 2: populate this when encoding == DICTIONARY
    std::vector<std::string> dictionary_;

    // ── Private decoders ────────────────────────────────────
    // Each returns the next decoded value for its encoding type.
    // next() dispatches to the right one based on encoding_ and type_.
    ColumnValue readNextNone();

    // TODO Phase 2: implement these
    ColumnValue readNextDict();
    ColumnValue readNextRLE();

    // ── Helpers ─────────────────────────────────────────────
    bool verifyAndLoadHeader();
    bool verifyCRC();
    void loadDictionary();  // TODO Phase 2
};

#endif // COLUMN_READER_H
