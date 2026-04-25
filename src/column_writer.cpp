#include "../include/column_writer.h"
#include <iostream>
#include <cstdio>

#ifdef _WIN32
    #include <direct.h>
    #include <windows.h>
#else
    #include <sys/stat.h>
    #include <unistd.h>
#endif

ColumnWriter::ColumnWriter(const std::string& table_dir,
                           const std::string& column_name,
                           ColumnType type,
                           Encoding encoding)
    : column_name_(column_name)
    , type_(type)
    , encoding_(encoding)
    , row_count_(0)
    , header_written_(false) {
    filepath_ = table_dir + "/" + column_name + ".col";
}

ColumnWriter::~ColumnWriter() {
    closeFile();
}

bool ColumnWriter::openFile() {
    // Extract directory path from filepath
    size_t slash_pos = filepath_.find_last_of("/\\");
    if (slash_pos != std::string::npos) {
        std::string dir = filepath_.substr(0, slash_pos);
        
        // Create directory recursively on Windows
        std::string cmd = "mkdir \"" + dir + "\" 2> nul";
        system(cmd.c_str());
    }
    
    file_.open(filepath_, std::ios::binary);
    return file_.is_open();
}

bool ColumnWriter::writeHeader() {
    if (!file_.is_open()) {
        if (!openFile()) {
            return false;
        }
    }
    
    ColumnHeader header;
    header.magic = MAGIC;
    header.version = VERSION;
    header.type = static_cast<uint8_t>(type_);
    header.encoding = static_cast<uint8_t>(encoding_);
    header.reserved = 0;
    header.row_count = row_count_;
    header.data_offset = HEADER_SIZE;
    
    file_.write(reinterpret_cast<const char*>(&header), HEADER_SIZE);
    if (file_.fail()) {
        return false;
    }
    
    header_written_ = true;
    return true;
}

void ColumnWriter::writeInt64Raw(int64_t value) {
    file_.write(reinterpret_cast<const char*>(&value), sizeof(int64_t));
}

void ColumnWriter::writeDoubleRaw(double value) {
    file_.write(reinterpret_cast<const char*>(&value), sizeof(double));
}

void ColumnWriter::writeStringRaw(const std::string& value) {
    uint64_t len = value.size();
    file_.write(reinterpret_cast<const char*>(&len), sizeof(uint64_t));
    file_.write(value.c_str(), len);
}

void ColumnWriter::setRowCount(size_t count) {
    row_count_ = count;
}

bool ColumnWriter::writeInt64(const std::vector<int64_t>& values) {
    if (!writeHeader()) {
        return false;
    }
    
    for (int64_t val : values) {
        writeInt64Raw(val);
        if (file_.fail()) {
            return false;
        }
    }
    
    return true;
}

bool ColumnWriter::writeDouble(const std::vector<double>& values) {
    if (!writeHeader()) {
        return false;
    }
    
    for (double val : values) {
        writeDoubleRaw(val);
        if (file_.fail()) {
            return false;
        }
    }
    
    return true;
}

bool ColumnWriter::writeString(const std::vector<std::string>& values) {
    if (!writeHeader()) {
        return false;
    }
    
    for (const auto& val : values) {
        writeStringRaw(val);
        if (file_.fail()) {
            return false;
        }
    }
    
    return true;
}

bool ColumnWriter::closeFile() {
    if (file_.is_open()) {
        file_.flush();
        file_.close();
    }
    return true;
}

bool ColumnWriter::finalize() {
    closeFile();
    return true;
}