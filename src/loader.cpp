#include "../include/csv_parser.h"
#include "../include/type_inference.h"
#include "../include/column_writer.h"
#include "../include/schema.h"
#include <iostream>
#include <string>
#include <cstdlib>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#define mkdir _mkdir
#else
#include <sys/stat.h>
#endif

class Loader {
public:
    static bool load(const std::string& csv_file, 
                     const std::string& table_name,
                     const std::string& warehouse_path) {
        
        std::cout << "Loading " << csv_file << " as " << table_name << "...\n";
        
        // 1. Parse CSV
        CSVParser parser(csv_file);
        if (!parser.parse()) {
            std::cerr << "Error parsing CSV: " << parser.getLastError() << "\n";
            return false;
        }
        
        auto headers = parser.getHeaders();
        auto rows = parser.getRows();
        
        std::cout << "  Parsed " << rows.size() << " rows, " << headers.size() << " columns\n";
        
        // 2. Infer types
        auto types = TypeInference::inferAllTypes(rows, headers.size());
        
        // 3. Create table directory
        std::string table_dir = warehouse_path + "/" + table_name;
        
        #ifdef _WIN32
        _mkdir(table_dir.c_str());
        #else
        mkdir(table_dir.c_str(), 0755);
        #endif
        
        // 4. Write each column
        for (size_t col_idx = 0; col_idx < headers.size(); col_idx++) {
            std::cout << "  Writing column: " << headers[col_idx] << " (";
            std::cout << typeToString(types[col_idx]) << ")\n";
            
            ColumnWriter writer(table_dir, headers[col_idx], 
                                types[col_idx], Encoding::NONE);
            
            writer.setRowCount(rows.size());
            
            if (types[col_idx] == ColumnType::INT64) {
                std::vector<int64_t> values;
                for (const auto& row : rows) {
                    if (col_idx < row.size() && !row[col_idx].empty()) {
                        values.push_back(std::stoll(row[col_idx]));
                    } else {
                        values.push_back(0);
                    }
                }
                if (!writer.writeInt64(values)) {
                    std::cerr << "Error writing int64 column\n";
                    return false;
                }
            } 
            else if (types[col_idx] == ColumnType::DOUBLE) {
                std::vector<double> values;
                for (const auto& row : rows) {
                    if (col_idx < row.size() && !row[col_idx].empty()) {
                        values.push_back(std::stod(row[col_idx]));
                    } else {
                        values.push_back(0.0);
                    }
                }
                if (!writer.writeDouble(values)) {
                    std::cerr << "Error writing double column\n";
                    return false;
                }
            }
            else {
                std::vector<std::string> values;
                for (const auto& row : rows) {
                    if (col_idx < row.size()) {
                        values.push_back(row[col_idx]);
                    } else {
                        values.push_back("");
                    }
                }
                if (!writer.writeString(values)) {
                    std::cerr << "Error writing string column\n";
                    return false;
                }
            }
            
            if (!writer.finalize()) {
                std::cerr << "Error finalizing column file\n";
                return false;
            }
        }
        
        // 5. Write schema (LAST — atomic completion signal)
        std::vector<SchemaColumn> schema_cols;
        for (size_t i = 0; i < headers.size(); i++) {
            schema_cols.push_back({headers[i], types[i], Encoding::NONE});
        }
        
        if (!SchemaManager::writeSchema(table_dir, schema_cols, table_name)) {
            std::cerr << "Error writing schema\n";
            return false;
        }
        
        std::cout << "Loaded " << rows.size() << " rows, " << headers.size() 
                  << " columns into " << table_dir << "/\n";
        
        return true;
    }
};

// Example usage (to be integrated into colsh by Person 3)
/*
int main(int argc, char* argv[]) {
    if (argc != 4) {
        std::cerr << "Usage: " << argv[0] << " <csv_file> <table_name> <warehouse_path>\n";
        return 1;
    }
    
    if (!Loader::load(argv[1], argv[2], argv[3])) {
        return 1;
    }
    
    return 0;
}
*/