#ifndef CSV_PARSER_H
#define CSV_PARSER_H

#include "common.h"
#include <string>
#include <vector>
#include <fstream>

// No changes needed to CSVParser header
class CSVParser {
public:
    explicit CSVParser(const std::string& filename);
    bool parse();
    std::vector<std::string> getHeaders() const;
    std::vector<Row> getRows() const;
    size_t getRowCount() const;
    std::string getLastError() const;

private:
    std::string filename_;
    std::vector<std::string> headers_;
    std::vector<Row> rows_;
    std::string last_error_;
    std::ifstream file_;
    std::vector<std::string> parseLine(const std::string& line);
};

#endif // CSV_PARSER_H
