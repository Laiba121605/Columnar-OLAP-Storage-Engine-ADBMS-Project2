#ifndef ROW_ENGINE_H
#define ROW_ENGINE_H

#include <string>
#include <vector>
#include <cstdint>

// Row structure matching the sales table
struct SalesRow {
    int64_t id;
    int32_t date;
    std::string country;
    std::string category;
    int32_t product_id;
    int32_t quantity;
    double price;
};

// Row-oriented storage engine (baseline)
class RowEngine {
public:
    // Write CSV to row-oriented binary file
    static bool load(const std::string& csv_file, const std::string& table_name,
                     const std::string& warehouse_path);
    
    // Query 1: SELECT SUM(price) WHERE date >= threshold
    static double query1SumPrice(const std::string& table_name,
                                  const std::string& warehouse_path,
                                  int32_t date_threshold,
                                  uint64_t& bytes_read);
    
    // Query 2: SELECT country, AVG(quantity) GROUP BY country
    static bool query2GroupByCountry(const std::string& table_name,
                                      const std::string& warehouse_path,
                                      std::vector<std::pair<std::string, double>>& results,
                                      uint64_t& bytes_read);
    
    // Query 3: SELECT * WHERE id = target (point lookup)
    static bool query3PointLookup(const std::string& table_name,
                                   const std::string& warehouse_path,
                                   int64_t target_id,
                                   SalesRow& result,
                                   uint64_t& bytes_read);
    
    // Get row count
    static uint64_t getRowCount(const std::string& table_name,
                                 const std::string& warehouse_path);
    
    // Get file size in bytes
    static uint64_t getFileSize(const std::string& table_name,
                                 const std::string& warehouse_path);
};

#endif // ROW_ENGINE_H