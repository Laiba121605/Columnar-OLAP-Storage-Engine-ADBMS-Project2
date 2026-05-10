#include <iostream>
#include <fstream>    // FIX (Issue 5): added missing include. processInfo() uses
                      // std::ifstream to read .col file sizes for the INFO command.
                      // Without this, compilation fails on stricter compilers that
                      // don't pull <fstream> in transitively via other headers.
#include <string>
#include <vector>
#include <sstream>
#include <deque>
#include <algorithm>
#include <iomanip>
#include "include/row_engine.h"
#include "include/loader.h"
#include "include/query_parser.h"
#include "include/predicate.h"
#include "include/column_reader.h"
#include "include/schema.h"
#include "include/executor.h"
#include "include/common.h"

// ============================================================
// BENCHMARK COMMAND
// ============================================================

#include <chrono>
#include <iomanip>

static void runBenchmark(const std::string& warehousePath) {
    std::cout << "\n========================================\n";
    std::cout << "RUNNING BENCHMARK\n";
    std::cout << "========================================\n\n";
    
    const std::string csv_file = "benchmark_sales.csv";
    const std::string table_name = "sales";
    
    // Step 1: Generate benchmark data if not exists
    std::ifstream check(csv_file);
    if (!check.good()) {
        std::cout << "Benchmark file not found. Please run: ./gen_benchmark.exe first\n";
        std::cout << "Generating benchmark dataset (1,000,000 rows)...\n";
        return;
    }
    check.close();
    
    // Step 2: Load into column store
    std::cout << "Loading into COLUMN store...\n";
    auto col_start = std::chrono::high_resolution_clock::now();
    Loader::load(csv_file, table_name, warehousePath);
    auto col_end = std::chrono::high_resolution_clock::now();
    auto col_load_ms = std::chrono::duration_cast<std::chrono::milliseconds>(col_end - col_start).count();
    std::cout << "Column store load time: " << col_load_ms << " ms\n\n";
    
    // Step 3: Load into row store
    std::cout << "Loading into ROW store...\n";
    auto row_start = std::chrono::high_resolution_clock::now();
    RowEngine::load(csv_file, table_name, warehousePath);
    auto row_end = std::chrono::high_resolution_clock::now();
    auto row_load_ms = std::chrono::duration_cast<std::chrono::milliseconds>(row_end - row_start).count();
    std::cout << "Row store load time: " << row_load_ms << " ms\n\n";
    
    // Get file sizes
    uint64_t col_size = 0;
    auto schema = SchemaManager::readSchema(warehousePath + "/" + table_name);
    for (const auto& col : schema) {
        std::string col_path = warehousePath + "/" + table_name + "/" + col.name + ".col";
        std::ifstream f(col_path, std::ios::binary | std::ios::ate);
        if (f.is_open()) {
            col_size += f.tellg();
        }
    }
    
    uint64_t row_size = RowEngine::getFileSize(table_name, warehousePath);
    
    std::cout << "Disk usage:\n";
    std::cout << "  Column store: " << std::fixed << std::setprecision(2) 
              << (col_size / (1024.0 * 1024.0)) << " MB\n";
    std::cout << "  Row store: " << (row_size / (1024.0 * 1024.0)) << " MB\n\n";
    
    // Get schema for column queries
    auto schema_sales = SchemaManager::readSchema(warehousePath + "/" + table_name);
    if (schema_sales.empty()) {
        std::cerr << "Error: Failed to read schema for table 'sales'\n";
        return;
    }
    
    // ============================================================
    // Query 1: SELECT SUM(price) WHERE date >= 20240101
    // ============================================================
    std::cout << "Query 1: SELECT SUM(price) FROM sales WHERE date >= 20240101\n";
    std::cout << "----------------------------------------\n";
    
    // Column store query 1
    auto q1_col_start = std::chrono::high_resolution_clock::now();
    std::string query1 = "SELECT SUM(price) FROM sales WHERE date >= 20240101";
    QueryParser parser1(query1);
    QueryPlan plan1;
    if (!parser1.parse(plan1)) {
        std::cerr << "Parse error: " << parser1.getError() << "\n";
        return;
    }
    
    auto cols1 = plan1.neededColumns();
    std::vector<ColumnReader> col_readers1(cols1.size());
    std::vector<ColumnReader*> col_ptrs1;
    for (size_t i = 0; i < cols1.size(); i++) {
        if (!col_readers1[i].open(warehousePath + "/" + table_name, cols1[i])) {
            std::cerr << "Failed to open column: " << cols1[i] << "\n";
            return;
        }
        col_ptrs1.push_back(&col_readers1[i]);
    }
    
    uint64_t rowCount = col_readers1[0].getRowCount();
    Bitmap bitmap1;
    if (plan1.where.has_where) {
        bitmap1 = PredicateEvaluator::evaluate(plan1.where, warehousePath + "/" + table_name, rowCount);
        if (bitmap1.empty()) {
            bitmap1 = Bitmap(rowCount, true);
        }
    } else {
        bitmap1 = Bitmap(rowCount, true);
    }
    
    Executor::run(plan1, bitmap1, col_ptrs1, cols1, schema_sales, warehousePath + "/" + table_name);
    auto q1_col_end = std::chrono::high_resolution_clock::now();
    auto q1_col_ms = std::chrono::duration_cast<std::chrono::milliseconds>(q1_col_end - q1_col_start).count();
    
    // Row store query 1
    auto q1_row_start = std::chrono::high_resolution_clock::now();
    uint64_t bytes_read1 = 0;
    double sum_price = RowEngine::query1SumPrice(table_name, warehousePath, 20240101, bytes_read1);
    auto q1_row_end = std::chrono::high_resolution_clock::now();
    auto q1_row_ms = std::chrono::duration_cast<std::chrono::milliseconds>(q1_row_end - q1_row_start).count();
    
    std::cout << "\nResults:\n";
    std::cout << "  Column store: " << q1_col_ms << " ms (read " << cols1.size() << " columns)\n";
    std::cout << "  Row store:    " << q1_row_ms << " ms (read all data)\n";
    double speedup1 = (q1_row_ms > 0) ? static_cast<double>(q1_row_ms) / q1_col_ms : 0;
    std::cout << "  Speedup: " << std::fixed << std::setprecision(2) << speedup1 << "x\n\n";
    
    // Clean up readers
    for (auto& r : col_readers1) r.close();
    
    // ============================================================
    // Query 2: SELECT country, AVG(quantity) GROUP BY country
    // ============================================================
    std::cout << "Query 2: SELECT country, AVG(quantity) FROM sales GROUP BY country\n";
    std::cout << "----------------------------------------\n";
    
    // Column store query 2
    auto q2_col_start = std::chrono::high_resolution_clock::now();
    std::string query2 = "SELECT country, AVG(quantity) FROM sales GROUP BY country";
    QueryParser parser2(query2);
    QueryPlan plan2;
    if (!parser2.parse(plan2)) {
        std::cerr << "Parse error: " << parser2.getError() << "\n";
        return;
    }
    
    auto cols2 = plan2.neededColumns();
    std::vector<ColumnReader> col_readers2(cols2.size());
    std::vector<ColumnReader*> col_ptrs2;
    for (size_t i = 0; i < cols2.size(); i++) {
        if (!col_readers2[i].open(warehousePath + "/" + table_name, cols2[i])) {
            std::cerr << "Failed to open column: " << cols2[i] << "\n";
            return;
        }
        col_ptrs2.push_back(&col_readers2[i]);
    }
    
    Bitmap bitmap2(rowCount, true);
    Executor::run(plan2, bitmap2, col_ptrs2, cols2, schema_sales, warehousePath + "/" + table_name);
    auto q2_col_end = std::chrono::high_resolution_clock::now();
    auto q2_col_ms = std::chrono::duration_cast<std::chrono::milliseconds>(q2_col_end - q2_col_start).count();
    
    // Row store query 2
    auto q2_row_start = std::chrono::high_resolution_clock::now();
    uint64_t bytes_read2 = 0;
    std::vector<std::pair<std::string, double>> group_results;
    RowEngine::query2GroupByCountry(table_name, warehousePath, group_results, bytes_read2);
    auto q2_row_end = std::chrono::high_resolution_clock::now();
    auto q2_row_ms = std::chrono::duration_cast<std::chrono::milliseconds>(q2_row_end - q2_row_start).count();
    
    std::cout << "\nResults:\n";
    std::cout << "  Column store: " << q2_col_ms << " ms\n";
    std::cout << "  Row store:    " << q2_row_ms << " ms\n";
    double speedup2 = (q2_row_ms > 0) ? static_cast<double>(q2_row_ms) / q2_col_ms : 0;
    std::cout << "  Speedup: " << std::fixed << std::setprecision(2) << speedup2 << "x\n\n";
    
    // Clean up readers
    for (auto& r : col_readers2) r.close();
    
    // Query 3: SELECT * WHERE id = 500000 (Point lookup)
std::cout << "Query 3: SELECT * FROM sales WHERE id = 500000\n";
std::cout << "----------------------------------------\n";

// Parse query to get WHERE clause
std::string query3 = "SELECT * FROM sales WHERE id = 500000";
QueryParser parser3(query3);
QueryPlan plan3;
if (!parser3.parse(plan3)) {
    std::cerr << "Parse error: " << parser3.getError() << "\n";
    return;
}

// For SELECT *, we need ALL columns from schema
std::vector<std::string> all_cols;
for (const auto& col : schema_sales) {
    all_cols.push_back(col.name);
}
std::cout << "Opening " << all_cols.size() << " columns for SELECT *\n";

// Open readers for ALL columns
std::vector<ColumnReader> col_readers3(all_cols.size());
std::vector<ColumnReader*> col_ptrs3;
for (size_t i = 0; i < all_cols.size(); i++) {
    if (!col_readers3[i].open(warehousePath + "/" + table_name, all_cols[i])) {
        std::cerr << "Failed to open column: " << all_cols[i] << "\n";
    } else {
        std::cout << "  Opened: " << all_cols[i] << "\n";
    }
    col_ptrs3.push_back(&col_readers3[i]);
}

auto q3_col_start = std::chrono::high_resolution_clock::now();

// Get row count
uint64_t rowCount3 = 0;
if (!col_readers3.empty()) {
    rowCount3 = col_readers3[0].getRowCount();
}

// Evaluate WHERE clause on the id column
Bitmap bitmap3;
if (plan3.where.has_where) {
    // Open a separate reader for the WHERE column (id)
    ColumnReader where_reader;
    if (where_reader.open(warehousePath + "/" + table_name, "id")) {
        bitmap3 = PredicateEvaluator::evaluateWithReader(plan3.where, where_reader);
        where_reader.close();
    }
    if (bitmap3.empty()) {
        bitmap3 = Bitmap(rowCount3, true);
    }
} else {
    bitmap3 = Bitmap(rowCount3, true);
}

// Create a plan for SELECT *
QueryPlan starPlan;
starPlan.select_star = true;
starPlan.table = table_name;
starPlan.where = plan3.where;

Executor::run(starPlan, bitmap3, col_ptrs3, all_cols, schema_sales, warehousePath + "/" + table_name);
auto q3_col_end = std::chrono::high_resolution_clock::now();
auto q3_col_ms = std::chrono::duration_cast<std::chrono::milliseconds>(q3_col_end - q3_col_start).count();

// Clean up readers
for (auto& r : col_readers3) r.close();
    
    std::cout << "\n========================================\n";
    std::cout << "BENCHMARK COMPLETE\n";
    std::cout << "========================================\n";
}
// Helper: trim whitespace from both ends
static std::string trim(const std::string& s) {
    size_t start = 0;
    while (start < s.size() && std::isspace(s[start])) start++;
    size_t end = s.size();
    while (end > start && std::isspace(s[end - 1])) end--;
    return s.substr(start, end - start);
}

// Helper: check if a string starts with a prefix (case-insensitive)
static bool startsWith(const std::string& str, const std::string& prefix) {
    if (str.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); i++) {
        if (std::toupper(str[i]) != std::toupper(prefix[i])) return false;
    }
    return true;
}

// Helper: extract the command keyword
static std::string getCommand(const std::string& line) {
    std::string trimmed = trim(line);
    std::string upper;
    for (char c : trimmed) {
        if (std::isspace(c)) break;
        upper += std::toupper(c);
    }
    return upper;
}

// ============================================================
// printUsage — help message
// ============================================================
static void printUsage() {
    std::cout << "\n";
    std::cout << "========================================\n";
    std::cout << "  colsh — Columnar OLAP Storage Engine\n";
    std::cout << "========================================\n";
    std::cout << "\n";
    std::cout << "Commands:\n";
    std::cout << "  LOAD <csv_file> AS <table_name>\n";
    std::cout << "  QUERY <sql_query>\n";
    std::cout << "  INFO <table_name>\n";
    std::cout << "  HELP\n";
    std::cout << "  \\quit\n";
    std::cout << "\n";
    std::cout << "Supported Queries:\n";
    std::cout << "  SELECT * FROM t WHERE col op val\n";
    std::cout << "  SELECT col1, col2 FROM t WHERE col op val\n";
    std::cout << "  SELECT SUM(col), COUNT(*) FROM t WHERE col op val\n";
    std::cout << "  SELECT col, SUM(val) FROM t GROUP BY col\n";
    std::cout << "\n";
}

// ============================================================
// processInfo — shows table schema and column sizes
// ============================================================
static void processInfo(const std::string& tableName, const std::string& warehousePath) {
    std::string tableDir = warehousePath + "/" + tableName;

    if (!SchemaManager::schemaExists(tableDir)) {
        std::cerr << "Error: Table '" << tableName << "' not found.\n";
        return;
    }

    auto schema = SchemaManager::readSchema(tableDir);
    std::cout << "\nTable: " << tableName << "\n";
    std::cout << "Columns: " << schema.size() << "\n\n";

    // Print header
    std::cout << std::left
              << std::setw(20) << "COLUMN"
              << std::setw(12) << "TYPE"
              << std::setw(14) << "ENCODING"
              << "SIZE\n";
    std::cout << std::string(60, '-') << "\n";

    double totalMB = 0.0;

    for (const auto& col : schema) {
        std::string filepath = tableDir + "/" + col.name + ".col";

        // std::ifstream is now safe to use here because <fstream> is included above.
        std::ifstream file(filepath, std::ios::binary | std::ios::ate);
        size_t sizeBytes = 0;
        if (file.is_open()) {
            sizeBytes = file.tellg();
            file.close();
        }

        double sizeMB = sizeBytes / (1024.0 * 1024.0);
        totalMB += sizeMB;

        std::cout << std::left
                  << std::setw(20) << col.name
                  << std::setw(12) << typeToString(col.type)
                  << std::setw(14) << encodingToString(col.encoding)
                  << std::fixed << std::setprecision(1) << sizeMB << " MB\n";
    }

    std::cout << std::string(60, '-') << "\n";
    std::cout << std::left << std::setw(46) << "TOTAL ON DISK:"
              << std::fixed << std::setprecision(1) << totalMB << " MB\n";
    std::cout << "\n";
}

// ============================================================
// processQuery — parse, plan, and execute a query
// ============================================================
static void processQuery(const std::string& queryText, const std::string& warehousePath) {
    // ── Step 1: Parse the query ─────────────────────────
    QueryParser parser(queryText);
    QueryPlan plan;

    if (!parser.parse(plan)) {
        std::cerr << "Parse error: " << parser.getError() << "\n";
        return;
    }

    std::string tableDir = warehousePath + "/" + plan.table;

    // Check if table exists
    if (!SchemaManager::schemaExists(tableDir)) {
        std::cerr << "Error: Table '" << plan.table << "' not found.\n";
        std::cerr << "Use LOAD to load a CSV first.\n";
        return;
    }

    // ── Step 2: Read schema ─────────────────────────────
    auto schema = SchemaManager::readSchema(tableDir);
    if (schema.empty()) {
        std::cerr << "Error: Could not read schema for table '" << plan.table << "'.\n";
        return;
    }

    // ── Step 3: Determine which columns to open ─────────
    std::vector<std::string> colsToOpen;

    if (plan.select_star) {
        // SELECT * — need ALL columns
        for (const auto& sc : schema) {
            colsToOpen.push_back(sc.name);
        }
    } else {
        colsToOpen = plan.neededColumns();
    }

    // COUNT(*) with no WHERE produces an empty colsToOpen.
    // We still need row_count, so open the first schema column as a sentinel.
    // The executor's COUNT path never reads values from it — only the bitmap size matters.
    bool count_star_only = false;
    if (colsToOpen.empty() && !plan.select_star) {
        if (!schema.empty()) {
            colsToOpen.push_back(schema[0].name);
            count_star_only = true;
        } else {
            std::cerr << "Error: No columns needed and schema is empty.\n";
            return;
        }
    }
    (void)count_star_only;

    // ── Step 4: Open column readers ─────────────────────
    std::vector<ColumnReader*> readers;
    std::deque<ColumnReader> readerStorage; // actual storage

    readerStorage.resize(colsToOpen.size());
    for (size_t i = 0; i < colsToOpen.size(); i++) {
        if (!readerStorage[i].open(tableDir, colsToOpen[i])) {
            std::cerr << "Error opening column '" << colsToOpen[i]
                      << "': " << readerStorage[i].getLastError() << "\n";
            // Close already-opened readers
            for (size_t j = 0; j < i; j++) {
                readerStorage[j].close();
            }
            return;
        }
        readers.push_back(&readerStorage[i]);
    }

    // ── Step 5: Evaluate predicate → bitmap ────────────
    Bitmap bitmap;
    uint64_t rowCount = 0;
    for (auto* r : readers) {
        if (r && r->isOpen()) {
            rowCount = r->getRowCount();
            break;
        }
    }

    if (plan.where.has_where) {
        // Check if the WHERE column is in our opened columns
        int whereIdx = -1;
        for (size_t i = 0; i < colsToOpen.size(); i++) {
            if (colsToOpen[i] == plan.where.col) {
                whereIdx = static_cast<int>(i);
                break;
            }
        }

        if (whereIdx >= 0) {
            // Use the already-opened reader to evaluate predicate
            bitmap = PredicateEvaluator::evaluateWithReader(plan.where, *readers[whereIdx]);
        } else {
            // WHERE column wasn't opened yet — open it temporarily
            bitmap = PredicateEvaluator::evaluate(plan.where, tableDir, rowCount);
        }

        if (bitmap.empty() && plan.where.has_where) {
            std::cerr << "Error evaluating predicate: " << PredicateEvaluator::getLastError() << "\n";
            for (auto& r : readerStorage) r.close();
            return;
        }
    } else {
        // No WHERE clause — all rows pass
        bitmap = Bitmap(rowCount, true);
    }

    // ── Step 6: Execute the query ──────────────────────
    bool success = Executor::run(plan, bitmap, readers, colsToOpen, schema, tableDir);

    if (!success) {
        std::cerr << "Error executing query.\n";
    }

    // ── Step 7: Cleanup ─────────────────────────────────
    for (auto& r : readerStorage) {
        r.close();
    }
}

// ============================================================
// processLoad — LOAD csv_file AS table_name
// ============================================================
static void processLoad(const std::string& args, const std::string& warehousePath) {
    // Expected format: <csv_file> AS <table_name>
    std::string trimmed = trim(args);

    // Find "AS" keyword (case-insensitive)
    std::string upper = trimmed;
    for (char& c : upper) c = std::toupper(c);

    size_t asPos = upper.find(" AS ");
    if (asPos == std::string::npos) {
        std::cerr << "Usage: LOAD <csv_file> AS <table_name>\n";
        return;
    }

    std::string csvFile = trim(trimmed.substr(0, asPos));
    std::string tableName = trim(trimmed.substr(asPos + 4)); // +4 to skip " AS "

    if (csvFile.empty() || tableName.empty()) {
        std::cerr << "Usage: LOAD <csv_file> AS <table_name>\n";
        return;
    }

    // Clean table name (remove quotes if present)
    if (tableName.front() == '"' && tableName.back() == '"') {
        tableName = tableName.substr(1, tableName.size() - 2);
    }
    if (tableName.front() == '\'' && tableName.back() == '\'') {
        tableName = tableName.substr(1, tableName.size() - 2);
    }

    Loader::load(csvFile, tableName, warehousePath);
}

// ============================================================
// main
// ============================================================
int main(int argc, char* argv[]) {
    std::string warehousePath = "./warehouse";

    // Parse command line arguments
    // Usage: ./colsh [-d data_directory]
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-d" && i + 1 < argc) {
            warehousePath = argv[++i];
        } else if (arg == "-h" || arg == "--help") {
            printUsage();
            return 0;
        }
    }

    std::cout << "Columnar OLAP Storage Engine\n";
    std::cout << "Warehouse: " << warehousePath << "\n";
    std::cout << "Type HELP for commands, \\quit to exit.\n\n";

    // ── Main REPL loop ─────────────────────────────────
    std::string line;
    while (true) {
        std::cout << "colsh> ";
        std::getline(std::cin, line);

        // Check for EOF
        if (std::cin.eof()) {
            std::cout << "\n";
            break;
        }

        std::string trimmed = trim(line);
        if (trimmed.empty()) continue;

        std::string cmd = getCommand(trimmed);

        // ── \\quit ─────────────────────────────────────
        if (cmd == "\\QUIT") {
            std::cout << "Goodbye.\n";
            break;
        }

        // ── HELP ───────────────────────────────────────
        else if (cmd == "HELP") {
            printUsage();
        }

        // ── LOAD ───────────────────────────────────────
        else if (cmd == "LOAD") {
            // Extract everything after "LOAD"
            std::string args = trim(trimmed.substr(4));
            processLoad(args, warehousePath);
        }

        // ── QUERY ──────────────────────────────────────
        else if (cmd == "QUERY") {
            // Extract everything after "QUERY"
            std::string queryText = trim(trimmed.substr(5));
            if (queryText.empty()) {
                std::cerr << "Usage: QUERY <sql_query>\n";
                continue;
            }
            processQuery(queryText, warehousePath);
        }

        // ── INFO ───────────────────────────────────────
        else if (cmd == "INFO") {
            std::string args = trim(trimmed.substr(4));
            if (args.empty()) {
                std::cerr << "Usage: INFO <table_name>\n";
                continue;
            }
            processInfo(args, warehousePath);
        }
        // ── BENCH ───────────────────────────────────────
else if (cmd == "BENCH") {
    std::string args = trim(trimmed.substr(5));
    if (args == "run") {
        runBenchmark(warehousePath);
    } else {
        std::cerr << "Usage: BENCH run\n";
    }
}

        // ── Unknown command ────────────────────────────
        else {
            std::cerr << "Unknown command: " << cmd << "\n";
            std::cerr << "Type HELP for available commands.\n";
        }
    }

    return 0;
}