#include "../include/loader.h"
#include "../include/csv_parser.h"
#include "../include/type_inference.h"
#include "../include/column_writer.h"
#include "../include/schema.h"
#include <iostream>
#include <string>

// Cross-platform mkdir
#ifdef _WIN32
    #include <direct.h>
    #define make_dir(p) _mkdir(p)
#else
    #include <sys/stat.h>
    #define make_dir(p) mkdir((p), 0755)
#endif

// BUG FIX 1: loader.cpp was redefining the Loader class from scratch instead of
// implementing its declared methods from loader.h. The class in loader.cpp was a
// local type that the linker never connected to the declaration in loader.h. Any
// translation unit calling Loader::load() would get an unresolved symbol at link time.
// FIX: include loader.h and implement Loader::load() as Loader::load().
//
// BUG FIX 2: loader.cpp had an unconditional #include <direct.h> (Windows-only)
// outside any #ifdef guard. On Linux (required by the manual: "builds on Ubuntu LTS")
// this is a compile error. Fixed by guarding it above.

bool Loader::load(const std::string& csv_file,
                  const std::string& table_name,
                  const std::string& warehouse_path) {

    std::cout << "Loading " << csv_file << " as " << table_name << "...\n";

    CSVParser parser(csv_file);
    if (!parser.parse()) {
        std::cerr << "Error parsing CSV: " << parser.getLastError() << "\n";
        return false;
    }

    auto headers = parser.getHeaders();
    auto rows    = parser.getRows();

    std::cout << "  Parsed " << rows.size() << " rows, "
              << headers.size() << " columns\n";

    auto types = TypeInference::inferAllTypes(rows, headers.size());

    std::string table_dir = warehouse_path + "/" + table_name;
    make_dir(table_dir.c_str());

    for (size_t col_idx = 0; col_idx < headers.size(); col_idx++) {
        std::cout << "  Writing column: " << headers[col_idx]
                  << " (" << typeToString(types[col_idx]) << ")\n";

        ColumnWriter writer(table_dir, headers[col_idx],
                            types[col_idx], Encoding::NONE);
        writer.setRowCount(rows.size());

        if (types[col_idx] == ColumnType::INT64) {
            std::vector<int64_t> values;
            for (const auto& row : rows) {
                if (col_idx < row.size() && !row[col_idx].empty())
                    values.push_back(std::stoll(row[col_idx]));
                else
                    values.push_back(0);
            }
            if (!writer.writeInt64(values)) return false;

        } else if (types[col_idx] == ColumnType::DOUBLE) {
            std::vector<double> values;
            for (const auto& row : rows) {
                if (col_idx < row.size() && !row[col_idx].empty())
                    values.push_back(std::stod(row[col_idx]));
                else
                    values.push_back(0.0);
            }
            if (!writer.writeDouble(values)) return false;

        } else {
            std::vector<std::string> values;
            for (const auto& row : rows) {
                if (col_idx < row.size())
                    values.push_back(row[col_idx]);
                else
                    values.push_back("");
            }
            if (!writer.writeString(values)) return false;
        }

        if (!writer.finalize()) return false;
    }

    std::vector<SchemaColumn> schema_cols;
    for (size_t i = 0; i < headers.size(); i++) {
        schema_cols.push_back({headers[i], types[i], Encoding::NONE});
    }

    if (!SchemaManager::writeSchema(table_dir, schema_cols, table_name))
        return false;

    std::cout << "Loaded " << rows.size() << " rows, " << headers.size()
              << " columns into " << table_dir << "/\n";
    return true;
}
