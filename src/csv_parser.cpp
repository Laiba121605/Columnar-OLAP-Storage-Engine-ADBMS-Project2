#include "../include/csv_parser.h"
#include <iostream>
#include <sstream>

// No changes to csv_parser.cpp
// Known limitation (Warning): multi-line fields inside quotes are not
// supported. getline() splits on every newline so a field containing
// a real newline would be split into two rows. Not an issue for the
// benchmark dataset which has no multi-line fields.

CSVParser::CSVParser(const std::string& filename) : filename_(filename) {}

bool CSVParser::parse() {
    file_.open(filename_);
    if (!file_.is_open()) {
        last_error_ = "Cannot open file: " + filename_;
        return false;
    }

    std::string line;
    bool first_line = true;

    while (std::getline(file_, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;

        std::vector<std::string> fields = parseLine(line);

        if (first_line) {
            headers_   = fields;
            first_line = false;
        } else {
            rows_.push_back(fields);
        }
    }

    file_.close();
    return true;
}

std::vector<std::string> CSVParser::getHeaders() const { return headers_; }
std::vector<Row>         CSVParser::getRows()    const { return rows_; }
size_t                   CSVParser::getRowCount() const { return rows_.size(); }
std::string              CSVParser::getLastError() const { return last_error_; }

std::vector<std::string> CSVParser::parseLine(const std::string& line) {
    std::vector<std::string> fields;
    std::string current;
    bool in_quotes = false;
    size_t i = 0;

    while (i < line.size()) {
        char c = line[i];

        if (c == '"') {
            if (in_quotes && i + 1 < line.size() && line[i + 1] == '"') {
                current += '"';
                i += 2;
                continue;
            }
            in_quotes = !in_quotes;
            i++;
        } else if (c == ',' && !in_quotes) {
            fields.push_back(current);
            current.clear();
            i++;
        } else {
            current += c;
            i++;
        }
    }
    fields.push_back(current);
    return fields;
}
