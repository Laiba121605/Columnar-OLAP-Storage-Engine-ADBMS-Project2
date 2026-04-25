#ifndef COLUMN_WRITER_H
#define COLUMN_WRITER_H

#include "common.h"
#include <string>
#include <vector>
#include <fstream>

class ColumnWriter {
public:
    ColumnWriter(const std::string& table_dir,
                 const std::string& column_name,
                 ColumnType type,
                 Encoding encoding);

    ~ColumnWriter();

    void setRowCount(size_t count);

    bool writeInt64(const std::vector<int64_t>& values);
    bool writeDouble(const std::vector<double>& values);
    bool writeString(const std::vector<std::string>& values);

    bool finalize();

private:
    // CHANGED: now tracks both temp path and final path
    // for the temp-file-and-rename pattern (Warning 1 fix)
    std::string filepath_temp_;   // write goes here first
    std::string filepath_final_;  // renamed to this on finalize()

    std::string column_name_;
    ColumnType  type_;
    Encoding    encoding_;
    std::ofstream file_;
    size_t      row_count_;
    bool        header_written_;

    // tracks data size as we write, needed for header (Issue 2 fix)
    uint64_t    data_size_written_;

    bool openFile();
    bool writeHeader();
    void writeInt64Raw(int64_t value);
    void writeDoubleRaw(double value);
    void writeStringRaw(const std::string& value);
    bool closeAndRename();
};

#endif // COLUMN_WRITER_H
