#ifndef COLUMN_READER_H
#define COLUMN_READER_H

// ============================================================
// column_reader.h
// ============================================================
// Bridge between storage files and the query engine.
// Phase 1: NONE encoding (raw values).
// Phase 2: DICTIONARY and RLE decoders added.
//
// Manual Section 6.1: "The iterator abstraction is what makes
// everything downstream uniform. The filter and the aggregator
// do not care how the column was encoded; they just pull values
// one at a time."
// ============================================================

#include "common.h"
#include <string>
#include <vector>
#include <fstream>
#include <variant>

using ColumnValue = std::variant<int64_t, double, std::string>;

class ColumnReader {
public:
    ColumnReader();
    ~ColumnReader();

    bool open(const std::string& table_dir, const std::string& column_name);
    bool hasNext() const;
    ColumnValue next();
    void reset();
    void close();

    ColumnType  getType()       const { return type_; }
    Encoding    getEncoding()   const { return encoding_; }
    uint64_t    getRowCount()   const { return row_count_; }
    std::string getColumnName() const { return column_name_; }
    bool        isOpen()        const { return is_open_; }
    std::string getLastError()  const { return last_error_; }

private:
    std::string   column_name_;
    std::string   filepath_;
    std::ifstream file_;
    ColumnType    type_;
    Encoding      encoding_;
    uint64_t      row_count_;
    uint64_t      data_size_;
    uint64_t      dict_size_;
    uint64_t      rows_read_;
    bool          is_open_;
    std::string   last_error_;
    std::streampos data_start_;

    // Phase 2 — DICTIONARY decoder state
    // dictionary_[id] = string value for that id.
    // Populated by loadDictionary() on open() when encoding == DICTIONARY.
    std::vector<std::string> dictionary_;

    // id_width_: how many bytes each dictionary id occupies in the data section.
    // 1 = uint8_t (D<=256), 2 = uint16_t (D<=65536), 4 = uint32_t.
    // Derived from D (dictionary_.size()) after loading the dictionary.
    uint8_t id_width_;

    // Phase 2 — RLE decoder state
    // The RLE iterator keeps the current run's value and how many
    // times it still needs to be emitted before reading the next pair.
    // Manual Section 6.1: "The iterator emits value length times
    // before advancing to the next pair."
    ColumnValue rle_current_value_;   // value of the current run
    uint64_t    rle_remaining_;       // rows left in this run (matches uint64_t on-disk run-length)
    bool        rle_run_loaded_;      // false until first pair is read

    // ── Private decoders ────────────────────────────────────
    ColumnValue readNextNone();
    ColumnValue readNextDict();   // Phase 2: implemented
    ColumnValue readNextRLE();    // Phase 2: implemented

    // ── Helpers ─────────────────────────────────────────────
    bool verifyAndLoadHeader();
    bool verifyCRC();
    void loadDictionary();        // Phase 2: implemented
};

#endif // COLUMN_READER_H