#pragma once
#include "common.h"
#include <string>
#include <vector>
#include <fstream>
#include <cstdio>

class ColumnWriter {
public:
    ColumnWriter(const std::string& table_dir,
                 const std::string& column_name,
                 ColumnType type,
                 Encoding encoding);
    ~ColumnWriter();

    void setRowCount(size_t count);

    // Phase 1: raw (NONE-encoded) write methods
    bool writeInt32 (const std::vector<int32_t>&     values);
    bool writeInt64 (const std::vector<int64_t>&     values);
    bool writeDouble(const std::vector<double>&      values);
    bool writeString(const std::vector<std::string>& values);

    // Phase 2: encoded write methods
    bool writeDictionary(const std::vector<std::string>& values);
    bool writeRLE_Int32 (const std::vector<int32_t>&     values);
    bool writeRLE_Int64 (const std::vector<int64_t>&     values);
    bool writeRLE_Double(const std::vector<double>&      values);

    bool finalize();

    Encoding getActualEncoding() const { return encoding_; }

private:
    std::string  column_name_;
    std::string  filepath_temp_;
    std::string  filepath_final_;
    ColumnType   type_;
    Encoding     encoding_;
    size_t       row_count_;
    bool         header_written_;
    uint64_t     data_size_written_;
    uint64_t     dict_size_written_;
    std::ofstream file_;   // Linux
#ifdef _WIN32
    FILE*        fp_ = nullptr;  // Windows: FILE* bypasses ofstream AV issues
#endif

    bool openFile();
    bool writeHeader();
    bool closeAndRename();

    void writeInt32Raw (int32_t             value);
    void writeInt64Raw (int64_t             value);
    void writeDoubleRaw(double              value);
    void writeStringRaw(const std::string&  value);

    void writeDictId   (uint32_t id, uint8_t id_width);
    void writeDictEntry(const std::string&  s);
};