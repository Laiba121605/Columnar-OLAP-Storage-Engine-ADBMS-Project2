#include "../include/loader.h"
#include "../include/csv_parser.h"
#include "../include/type_inference.h"
#include "../include/column_writer.h"
#include "../include/schema.h"
#include <iostream>
#include <fstream>
#include <iomanip>
#include <string>
#include <unordered_set>

#ifdef _WIN32
    #include <direct.h>
    #define make_dir(p) _mkdir(p)
#else
    #include <sys/stat.h>
    #define make_dir(p) mkdir((p), 0755)
#endif

// ============================================================
// Phase 2: chooseEncoding()
// Decides which encoding to use for a column.
// Manual Section 5.1 rules (deterministic):
//
//   STRING  -> DICTIONARY  (unless D > 65535, then NONE)
//   NUMERIC -> RLE         if run_count < N/4
//   NUMERIC -> NONE        otherwise
// ============================================================
static Encoding chooseEncoding(ColumnType type,
                                const std::vector<std::string>& raw_values) {
    size_t N = raw_values.size();
    if (N == 0) return Encoding::NONE;

    if (type == ColumnType::STRING) {
        std::unordered_set<std::string> distinct(raw_values.begin(), raw_values.end());
        if (distinct.size() <= 65535)
            return Encoding::DICTIONARY;
        return Encoding::NONE;
    }

    // Numeric: count runs of identical raw strings
    // (comparing strings avoids floating-point equality issues)
    size_t run_count = 1;
    for (size_t i = 1; i < N; i++) {
        if (raw_values[i] != raw_values[i - 1]) run_count++;
    }

    if (run_count < N / 4)
        return Encoding::RLE;

    return Encoding::NONE;
}

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

    std::vector<SchemaColumn> schema_cols;

    for (size_t col_idx = 0; col_idx < headers.size(); col_idx++) {

        // Collect raw string values for this column
        std::vector<std::string> raw_values;
        raw_values.reserve(rows.size());
        for (const auto& row : rows) {
            if (col_idx < row.size())
                raw_values.push_back(row[col_idx]);
            else
                raw_values.push_back("");
        }

        // Phase 2: choose encoding based on column type and data pattern
        Encoding enc = chooseEncoding(types[col_idx], raw_values);

        ColumnWriter writer(table_dir, headers[col_idx], types[col_idx], enc);
        writer.setRowCount(rows.size());

        bool ok = false;

        if (types[col_idx] == ColumnType::INT64) {
            std::vector<int64_t> values;
            values.reserve(rows.size());
            for (const auto& rv : raw_values) {
                if (!rv.empty()) values.push_back(std::stoll(rv));
                else             values.push_back(0);
            }
            ok = (enc == Encoding::RLE) ? writer.writeRLE_Int64(values)
                                        : writer.writeInt64(values);

        } else if (types[col_idx] == ColumnType::DOUBLE) {
            std::vector<double> values;
            values.reserve(rows.size());
            for (const auto& rv : raw_values) {
                if (!rv.empty()) values.push_back(std::stod(rv));
                else             values.push_back(0.0);
            }
            ok = (enc == Encoding::RLE) ? writer.writeRLE_Double(values)
                                        : writer.writeDouble(values);

        } else {
            // STRING
            ok = (enc == Encoding::DICTIONARY) ? writer.writeDictionary(raw_values)
                                               : writer.writeString(raw_values);
        }

        if (!ok) return false;
        if (!writer.finalize()) return false;

        schema_cols.push_back({headers[col_idx], types[col_idx], enc});
    }

    if (!SchemaManager::writeSchema(table_dir, schema_cols, table_name))
        return false;

    // Per-column size table
    std::cout << "\n";
    std::cout << std::left
              << std::setw(20) << "column name"
              << std::setw(10) << "type"
              << std::setw(12) << "encoding"
              << "size\n";
    std::cout << std::string(54, '-') << "\n";

    double total_col_mb = 0.0;
    for (size_t i = 0; i < headers.size(); i++) {
        std::string col_path = table_dir + "/" + headers[i] + ".col";
        std::ifstream col_file(col_path, std::ios::binary | std::ios::ate);
        double size_mb = 0.0;
        if (col_file.is_open()) {
            size_mb = static_cast<double>(col_file.tellg()) / (1024.0 * 1024.0);
            col_file.close();
        }
        total_col_mb += size_mb;

        std::cout << std::left
                  << std::setw(20) << headers[i]
                  << std::setw(10) << typeToString(types[i])
                  << std::setw(12) << encodingToString(schema_cols[i].encoding)
                  << std::fixed << std::setprecision(1) << size_mb << " MB\n";
    }

    std::cout << std::string(54, '-') << "\n";

    double csv_mb = 0.0;
    {
        std::ifstream f(csv_file, std::ios::binary | std::ios::ate);
        if (f.is_open()) { csv_mb = static_cast<double>(f.tellg()) / (1024.0*1024.0); }
    }

    std::cout << "total on disk:     "
              << std::fixed << std::setprecision(1) << total_col_mb << " MB\n";
    std::cout << "original CSV size: "
              << std::fixed << std::setprecision(1) << csv_mb << " MB\n";
    if (total_col_mb > 0.0)
        std::cout << "compression ratio: "
                  << std::fixed << std::setprecision(2)
                  << (csv_mb / total_col_mb) << "x\n";

    std::cout << "\nLoaded " << rows.size() << " rows, " << headers.size()
              << " columns into " << table_dir << "/\n";
    return true;
}