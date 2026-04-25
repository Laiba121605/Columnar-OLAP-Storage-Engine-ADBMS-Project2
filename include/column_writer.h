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
    std::string filepath_;
    std::string column_name_;
    ColumnType type_;
    Encoding encoding_;
    std::ofstream file_;
    size_t row_count_;
    bool header_written_;
    
    bool openFile();
    bool writeHeader();
    void writeInt64Raw(int64_t value);
    void writeDoubleRaw(double value);
    void writeStringRaw(const std::string& value);
    bool closeFile();
};

#endif // COLUMN_WRITER_H