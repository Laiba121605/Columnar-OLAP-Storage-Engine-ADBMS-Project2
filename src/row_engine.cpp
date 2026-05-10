#include "../include/row_engine.h"
#include "../include/csv_parser.h"
#include <fstream>
#include <iostream>
#include <map>
#include <cstring>

#ifdef _WIN32
    #include <direct.h>
    #define mkdir(dir) _mkdir(dir)
#else
    #include <sys/stat.h>
    #define mkdir(dir) mkdir(dir, 0755)
#endif

// Helper: create directory
static void createDir(const std::string& path) {
#ifdef _WIN32
    std::string cmd = "mkdir " + path + " 2> nul";
    system(cmd.c_str());
#else
    mkdir(path.c_str());
#endif
}

// ============================================================
// Load CSV into row-oriented binary file
// Format: [id(int64)][date(int32)][country_len(uint16)][country_bytes][category_len(uint16)][category_bytes][product_id(int32)][quantity(int32)][price(double)]
// ============================================================
bool RowEngine::load(const std::string& csv_file, const std::string& table_name,
                     const std::string& warehouse_path) {
    
    std::string table_dir = warehouse_path + "/row_" + table_name;
    
    // Create directory using Windows command (more reliable)
#ifdef _WIN32
    std::string cmd = "if not exist \"" + table_dir + "\" mkdir \"" + table_dir + "\"";
    system(cmd.c_str());
    // Also ensure parent exists
    cmd = "if not exist \"" + warehouse_path + "\" mkdir \"" + warehouse_path + "\"";
    system(cmd.c_str());
#else
    mkdir(table_dir.c_str(), 0755);
#endif
    
    // Verify directory exists
    std::ifstream dir_check(table_dir + "/.");
    if (!dir_check.good()) {
        std::cerr << "RowEngine: Failed to create directory " << table_dir << "\n";
        return false;
    }
    dir_check.close();
    
    std::string row_file = table_dir + "/data.bin";
    std::string temp_file = row_file + ".tmp";
    
    // Parse CSV
    CSVParser parser(csv_file);
    if (!parser.parse()) {
        std::cerr << "RowEngine: Failed to parse CSV: " << parser.getLastError() << "\n";
        return false;
    }
    
    auto headers = parser.getHeaders();
    auto rows = parser.getRows();
    
    // Find column indices
    int id_idx = -1, date_idx = -1, country_idx = -1, category_idx = -1;
    int product_idx = -1, quantity_idx = -1, price_idx = -1;
    
    for (size_t i = 0; i < headers.size(); i++) {
        std::string h = headers[i];
        if (h == "id") id_idx = i;
        else if (h == "date") date_idx = i;
        else if (h == "country") country_idx = i;
        else if (h == "category") category_idx = i;
        else if (h == "product_id") product_idx = i;
        else if (h == "quantity") quantity_idx = i;
        else if (h == "price") price_idx = i;
    }
    
    // Open temp file
    std::ofstream file(temp_file, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "RowEngine: Cannot create " << temp_file << "\n";
        std::cerr << "Check if directory '" << table_dir << "' exists and is writable\n";
        return false;
    }
    
    // Write each row
    for (const auto& row : rows) {
        SalesRow sr;
        sr.id = (id_idx >= 0 && !row[id_idx].empty()) ? std::stoll(row[id_idx]) : 0;
        sr.date = (date_idx >= 0 && !row[date_idx].empty()) ? std::stoi(row[date_idx]) : 0;
        sr.country = (country_idx >= 0) ? row[country_idx] : "";
        sr.category = (category_idx >= 0) ? row[category_idx] : "";
        sr.product_id = (product_idx >= 0 && !row[product_idx].empty()) ? std::stoi(row[product_idx]) : 0;
        sr.quantity = (quantity_idx >= 0 && !row[quantity_idx].empty()) ? std::stoi(row[quantity_idx]) : 0;
        sr.price = (price_idx >= 0 && !row[price_idx].empty()) ? std::stod(row[price_idx]) : 0.0;
        
        // Write binary
        file.write(reinterpret_cast<const char*>(&sr.id), sizeof(int64_t));
        file.write(reinterpret_cast<const char*>(&sr.date), sizeof(int32_t));
        
        uint16_t country_len = static_cast<uint16_t>(sr.country.size());
        file.write(reinterpret_cast<const char*>(&country_len), sizeof(uint16_t));
        file.write(sr.country.c_str(), country_len);
        
        uint16_t category_len = static_cast<uint16_t>(sr.category.size());
        file.write(reinterpret_cast<const char*>(&category_len), sizeof(uint16_t));
        file.write(sr.category.c_str(), category_len);
        
        file.write(reinterpret_cast<const char*>(&sr.product_id), sizeof(int32_t));
        file.write(reinterpret_cast<const char*>(&sr.quantity), sizeof(int32_t));
        file.write(reinterpret_cast<const char*>(&sr.price), sizeof(double));
    }
    
    file.close();
    
    // Rename temp to final
    std::remove(row_file.c_str());
    if (std::rename(temp_file.c_str(), row_file.c_str()) != 0) {
        std::cerr << "RowEngine: Failed to rename " << temp_file << " to " << row_file << "\n";
        return false;
    }
    
    // Verify file was created
    std::ifstream verify(row_file, std::ios::binary | std::ios::ate);
    if (!verify.is_open()) {
        std::cerr << "RowEngine: File was not created: " << row_file << "\n";
        return false;
    }
    uint64_t file_size = verify.tellg();
    verify.close();
    
    std::cout << "RowEngine: Loaded " << rows.size() << " rows into " << row_file 
              << " (" << (file_size / (1024.0 * 1024.0)) << " MB)\n";
    return true;
}
// ============================================================
// Read a single row from file at given position
// ============================================================
static bool readRowFromFile(std::ifstream& file, SalesRow& row) {
    file.read(reinterpret_cast<char*>(&row.id), sizeof(int64_t));
    if (file.fail()) return false;
    
    file.read(reinterpret_cast<char*>(&row.date), sizeof(int32_t));
    
    uint16_t country_len;
    file.read(reinterpret_cast<char*>(&country_len), sizeof(uint16_t));
    row.country.resize(country_len);
    file.read(&row.country[0], country_len);
    
    uint16_t category_len;
    file.read(reinterpret_cast<char*>(&category_len), sizeof(uint16_t));
    row.category.resize(category_len);
    file.read(&row.category[0], category_len);
    
    file.read(reinterpret_cast<char*>(&row.product_id), sizeof(int32_t));
    file.read(reinterpret_cast<char*>(&row.quantity), sizeof(int32_t));
    file.read(reinterpret_cast<char*>(&row.price), sizeof(double));
    
    return true;
}

// ============================================================
// Query 1: SELECT SUM(price) WHERE date >= threshold
// ============================================================
double RowEngine::query1SumPrice(const std::string& table_name,
                                  const std::string& warehouse_path,
                                  int32_t date_threshold,
                                  uint64_t& bytes_read) {
    
    std::string row_file = warehouse_path + "/row_" + table_name + "/data.bin";
    std::ifstream file(row_file, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "RowEngine: Cannot open " << row_file << "\n";
        return 0.0;
    }
    
    double sum = 0.0;
    SalesRow row;
    bytes_read = 0;
    
    while (readRowFromFile(file, row)) {
        bytes_read += sizeof(int64_t) + sizeof(int32_t) + sizeof(uint16_t) + row.country.size() +
                      sizeof(uint16_t) + row.category.size() + sizeof(int32_t) + sizeof(int32_t) + sizeof(double);
        
        if (row.date >= date_threshold) {
            sum += row.price;
        }
    }
    
    file.close();
    return sum;
}

// ============================================================
// Query 2: SELECT country, AVG(quantity) GROUP BY country
// ============================================================
bool RowEngine::query2GroupByCountry(const std::string& table_name,
                                      const std::string& warehouse_path,
                                      std::vector<std::pair<std::string, double>>& results,
                                      uint64_t& bytes_read) {
    
    std::string row_file = warehouse_path + "/row_" + table_name + "/data.bin";
    std::ifstream file(row_file, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "RowEngine: Cannot open " << row_file << "\n";
        return false;
    }
    
    std::map<std::string, std::pair<int64_t, int64_t>> aggregates; // country -> (sum_quantity, count)
    SalesRow row;
    bytes_read = 0;
    
    while (readRowFromFile(file, row)) {
        bytes_read += sizeof(int64_t) + sizeof(int32_t) + sizeof(uint16_t) + row.country.size() +
                      sizeof(uint16_t) + row.category.size() + sizeof(int32_t) + sizeof(int32_t) + sizeof(double);
        
        aggregates[row.country].first += row.quantity;
        aggregates[row.country].second++;
    }
    
    file.close();
    
    results.clear();
    for (const auto& [country, agg] : aggregates) {
        double avg = static_cast<double>(agg.first) / agg.second;
        results.emplace_back(country, avg);
    }
    
    return true;
}

// ============================================================
// Query 3: SELECT * WHERE id = target (point lookup)
// ============================================================
bool RowEngine::query3PointLookup(const std::string& table_name,
                                   const std::string& warehouse_path,
                                   int64_t target_id,
                                   SalesRow& result,
                                   uint64_t& bytes_read) {
    
    std::string row_file = warehouse_path + "/row_" + table_name + "/data.bin";
    std::ifstream file(row_file, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "RowEngine: Cannot open " << row_file << "\n";
        return false;
    }
    
    SalesRow row;
    bytes_read = 0;
    uint64_t row_num = 0;
    
    while (readRowFromFile(file, row)) {
        bytes_read += sizeof(int64_t) + sizeof(int32_t) + sizeof(uint16_t) + row.country.size() +
                      sizeof(uint16_t) + row.category.size() + sizeof(int32_t) + sizeof(int32_t) + sizeof(double);
        
        if (row.id == target_id) {
            result = row;
            file.close();
            return true;
        }
        row_num++;
    }
    
    file.close();
    return false;
}

// ============================================================
// Get row count
// ============================================================
uint64_t RowEngine::getRowCount(const std::string& table_name,
                                 const std::string& warehouse_path) {
    
    std::string row_file = warehouse_path + "/row_" + table_name + "/data.bin";
    std::ifstream file(row_file, std::ios::binary);
    if (!file.is_open()) return 0;
    
    file.seekg(0, std::ios::end);
    uint64_t file_size = file.tellg();
    file.close();
    
    // Estimate based on average row size (~variable due to strings)
    // For simplicity, we'll read and count
    std::ifstream count_file(row_file, std::ios::binary);
    SalesRow row;
    uint64_t count = 0;
    while (readRowFromFile(count_file, row)) {
        count++;
    }
    return count;
}

// ============================================================
// Get file size
// ============================================================
uint64_t RowEngine::getFileSize(const std::string& table_name,
                                 const std::string& warehouse_path) {
    
    std::string row_file = warehouse_path + "/row_" + table_name + "/data.bin";
    std::ifstream file(row_file, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        std::cerr << "RowEngine: Cannot open " << row_file << " for size check\n";
        return 0;
    }
    uint64_t size = file.tellg();
    file.close();
    return size;
}