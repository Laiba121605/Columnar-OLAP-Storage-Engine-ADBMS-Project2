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

#include "include/loader.h"
#include "include/query_parser.h"
#include "include/predicate.h"
#include "include/column_reader.h"
#include "include/schema.h"
#include "include/executor.h"
#include "include/common.h"

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

        // ── Unknown command ────────────────────────────
        else {
            std::cerr << "Unknown command: " << cmd << "\n";
            std::cerr << "Type HELP for available commands.\n";
        }
    }

    return 0;
}