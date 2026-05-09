// ============================================================
// test_phase1.cpp — Comprehensive Phase 1 Test Suite
//
// Tests every component in the engine:
//   1.  CRC64               (common.cpp)
//   2.  Type converters      (common.cpp)
//   3.  TypeInference        (type_inference.cpp)
//   4.  CSVParser            (csv_parser.cpp)
//   5.  ColumnWriter         (column_writer.cpp)
//   6.  ColumnReader         (column_reader.cpp)
//   7.  SchemaManager        (schema.cpp)
//   8.  QueryParser          (query_parser.cpp)
//   9.  PredicateEvaluator   (predicate.cpp)
//  10.  End-to-end roundtrip  (write + read + query)
//
// No external test framework is used — everything is plain C++.
// Each test prints PASS or FAIL with a description.
// At the end a summary line shows total passed / failed.
//
// HOW TO COMPILE (command line):
//   g++ -std=c++17 -o test_phase1 test_phase1.cpp \
//       src/common.cpp src/type_inference.cpp src/csv_parser.cpp \
//       src/column_writer.cpp src/column_reader.cpp src/schema.cpp \
//       src/query_parser.cpp src/predicate.cpp
//
// HOW TO RUN:
//   ./test_phase1
//
// HOW TO RUN IN VS CODE — see the long comment at the bottom of this
// file for full step-by-step VS Code instructions.
// ============================================================

#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <variant>
#include <cassert>
#include <cstdio>      // remove()
#include <filesystem>  // std::filesystem::remove_all (C++17)

// Pull in the engine headers — adjust paths to match your project layout.
// If your headers are directly in "include/", change these to:
//   #include "include/common.h"  etc.
#include "include/common.h"
#include "include/type_inference.h"
#include "include/csv_parser.h"
#include "include/column_writer.h"
#include "include/column_reader.h"
#include "include/schema.h"
#include "include/query_parser.h"
#include "include/predicate.h"

// ============================================================
// Tiny test harness — no dependencies, no macros magic.
// Just two counters and a helper that prints PASS/FAIL.
// ============================================================
static int g_passed = 0;
static int g_failed = 0;

// CHECK(condition, description)
// Prints "[PASS]" or "[FAIL]" with the description and updates counters.
static void CHECK(bool condition, const std::string& description) {
    if (condition) {
        std::cout << "  [PASS] " << description << "\n";
        g_passed++;
    } else {
        std::cout << "  [FAIL] " << description << "\n";
        g_failed++;
    }
}

// SECTION(name) — prints a header line so output is easy to skim
static void SECTION(const std::string& name) {
    std::cout << "\n-- " << name << " --\n";
}

// ============================================================
// Helpers shared across multiple test sections
// ============================================================

// Write a small CSV file to disk so CSVParser and Loader tests have input.
static void writeTempCSV(const std::string& path, const std::string& content) {
    std::ofstream f(path);
    f << content;
    f.close();
}

// Recursively delete a directory (test teardown).
// Works on Windows (MSVC), Linux, and macOS.
static void removeDir(const std::string& path) {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
    // ignore errors - directory may not exist
}

// ensureBaseDir() - creates ./test_tmp/ before any test runs.
// On Windows /tmp/ does not exist so we use a local folder instead.
// Called once at the top of main().
static void ensureBaseDir() {
    std::error_code ec;
    std::filesystem::create_directories("./test_tmp", ec);
}

// ============================================================
// 1. CRC64 tests
// ============================================================
static void test_crc64() {
    SECTION("CRC64 (common.cpp)");

    // Empty buffer should produce a known value (all-zero input, CRC of nothing is 0).
    uint64_t empty_crc = crc64(nullptr, 0);
    CHECK(empty_crc == 0, "CRC64 of empty buffer is 0");

    // Same data must always produce the same CRC — determinism check.
    const char* data = "hello world";
    uint64_t crc_a = crc64(data, 11);
    uint64_t crc_b = crc64(data, 11);
    CHECK(crc_a == crc_b, "CRC64 is deterministic for same input");

    // Different data must produce different CRCs (collision would be extraordinary).
    uint64_t crc_c = crc64("hello World", 11);  // capital W
    CHECK(crc_a != crc_c, "CRC64 differs for different input");

    // Incremental update must match one-shot.
    // This is what column_writer.cpp relies on when it patches the header
    // and then re-reads the file to compute CRC.
    uint64_t incremental = 0;
    incremental = crc64_update(incremental, "hello", 5);
    incremental = crc64_update(incremental, " world", 6);
    CHECK(incremental == crc_a, "CRC64 incremental update matches one-shot");

    // Single-byte boundary: CRC of one byte should be non-zero.
    const char one = 0x42;
    CHECK(crc64(&one, 1) != 0, "CRC64 of single non-zero byte is non-zero");
}

// ============================================================
// 2. Type converter tests (typeToString, stringToType, etc.)
// ============================================================
static void test_type_converters() {
    SECTION("Type / Encoding converters (common.cpp)");

    // typeToString round-trips
    CHECK(std::string(typeToString(ColumnType::INT32))  == "int32",  "typeToString INT32");
    CHECK(std::string(typeToString(ColumnType::INT64))  == "int64",  "typeToString INT64");
    CHECK(std::string(typeToString(ColumnType::DOUBLE)) == "double", "typeToString DOUBLE");
    CHECK(std::string(typeToString(ColumnType::STRING)) == "string", "typeToString STRING");

    // stringToType round-trips
    CHECK(stringToType("int32")  == ColumnType::INT32,  "stringToType int32");
    CHECK(stringToType("int64")  == ColumnType::INT64,  "stringToType int64");
    CHECK(stringToType("double") == ColumnType::DOUBLE, "stringToType double");
    CHECK(stringToType("string") == ColumnType::STRING, "stringToType string");
    CHECK(stringToType("banana") == ColumnType::STRING, "stringToType unknown falls back to STRING");

    // encodingToString round-trips
    CHECK(std::string(encodingToString(Encoding::NONE))       == "none",       "encodingToString NONE");
    CHECK(std::string(encodingToString(Encoding::DICTIONARY)) == "dictionary", "encodingToString DICTIONARY");
    CHECK(std::string(encodingToString(Encoding::RLE))        == "rle",        "encodingToString RLE");

    // stringToEncoding round-trips
    CHECK(stringToEncoding("none")       == Encoding::NONE,       "stringToEncoding none");
    CHECK(stringToEncoding("dictionary") == Encoding::DICTIONARY, "stringToEncoding dictionary");
    CHECK(stringToEncoding("rle")        == Encoding::RLE,        "stringToEncoding rle");
    CHECK(stringToEncoding("unknown")    == Encoding::NONE,       "stringToEncoding unknown falls back to NONE");
}

// ============================================================
// 3. TypeInference tests
// ============================================================
static void test_type_inference() {
    SECTION("TypeInference (type_inference.cpp)");

    // isInt64 — valid integers
    CHECK(TypeInference::isInt64("0"),           "isInt64: 0");
    CHECK(TypeInference::isInt64("42"),          "isInt64: 42");
    CHECK(TypeInference::isInt64("-999"),         "isInt64: -999");
    CHECK(TypeInference::isInt64("9223372036854775807"), "isInt64: INT64_MAX");

    // isInt64 — invalid
    CHECK(!TypeInference::isInt64(""),           "isInt64: empty string is false");
    CHECK(!TypeInference::isInt64("3.14"),        "isInt64: float is false");
    CHECK(!TypeInference::isInt64("abc"),         "isInt64: letters is false");
    CHECK(!TypeInference::isInt64("1e5"),         "isInt64: scientific notation is false");

    // isDouble — valid
    CHECK(TypeInference::isDouble("3.14"),        "isDouble: 3.14");
    CHECK(TypeInference::isDouble("1e5"),         "isDouble: 1e5");
    CHECK(TypeInference::isDouble("-0.001"),       "isDouble: -0.001");
    CHECK(TypeInference::isDouble("42"),          "isDouble: integer 42 is also a valid double");

    // isDouble — invalid
    CHECK(!TypeInference::isDouble(""),           "isDouble: empty string is false");
    CHECK(!TypeInference::isDouble("abc"),         "isDouble: letters is false");

    // inferType — all integers -> INT64
    CHECK(TypeInference::inferType({"1", "2", "3"}) == ColumnType::INT64,
          "inferType: all integers -> INT64");

    // inferType — mix of int and float -> DOUBLE
    CHECK(TypeInference::inferType({"1", "2.5", "3"}) == ColumnType::DOUBLE,
          "inferType: int+float mix -> DOUBLE");

    // inferType — has a string -> STRING
    CHECK(TypeInference::inferType({"1", "hello", "3"}) == ColumnType::STRING,
          "inferType: has string -> STRING");

    // inferType — empty cells are skipped (the bug fix)
    CHECK(TypeInference::inferType({"99.9", "", "14.5"}) == ColumnType::DOUBLE,
          "inferType: empty cells skipped, still DOUBLE");
    CHECK(TypeInference::inferType({"10", "", "20"}) == ColumnType::INT64,
          "inferType: empty cells skipped, still INT64");

    // inferType — all empty cells: every cell is skipped by the empty-cell guard,
    // so all_int and all_double both stay true and the function returns INT64.
    // This is the actual behaviour of the code (see type_inference.cpp inferType()).
    CHECK(TypeInference::inferType({"", "", ""}) == ColumnType::INT64,
          "inferType: all empty cells -> INT64 (all_int flag never cleared)");

    // inferType — empty vector -> STRING
    CHECK(TypeInference::inferType({}) == ColumnType::STRING,
          "inferType: empty vector -> STRING");

    // inferAllTypes — multi-column
    std::vector<Row> rows = {
        {"1", "3.14", "hello"},
        {"2", "2.71", "world"},
    };
    auto types = TypeInference::inferAllTypes(rows, 3);
    CHECK(types[0] == ColumnType::INT64,  "inferAllTypes: col0 -> INT64");
    CHECK(types[1] == ColumnType::DOUBLE, "inferAllTypes: col1 -> DOUBLE");
    CHECK(types[2] == ColumnType::STRING, "inferAllTypes: col2 -> STRING");
}

// ============================================================
// 4. CSVParser tests
// ============================================================
static void test_csv_parser() {
    SECTION("CSVParser (csv_parser.cpp)");

    // --- basic CSV ---
    writeTempCSV("./test_tmp/test_basic.csv",
        "id,name,price\n"
        "1,Alice,9.99\n"
        "2,Bob,14.50\n"
        "3,Charlie,4.00\n"
    );
    {
        CSVParser p("./test_tmp/test_basic.csv");
        CHECK(p.parse(), "basic CSV parses without error");
        auto headers = p.getHeaders();
        CHECK(headers.size() == 3,           "basic CSV: 3 headers");
        CHECK(headers[0] == "id",            "basic CSV: header[0] == id");
        CHECK(headers[1] == "name",          "basic CSV: header[1] == name");
        CHECK(headers[2] == "price",         "basic CSV: header[2] == price");
        CHECK(p.getRowCount() == 3,          "basic CSV: 3 data rows");
        auto rows = p.getRows();
        CHECK(rows[0][0] == "1",             "basic CSV: row0 col0 == 1");
        CHECK(rows[1][1] == "Bob",           "basic CSV: row1 col1 == Bob");
        CHECK(rows[2][2] == "4.00",          "basic CSV: row2 col2 == 4.00");
    }

    // --- quoted fields with embedded comma ---
    writeTempCSV("./test_tmp/test_quoted.csv",
        "id,city,note\n"
        "1,\"New York, NY\",ok\n"
        "2,London,fine\n"
    );
    {
        CSVParser p("./test_tmp/test_quoted.csv");
        CHECK(p.parse(), "quoted CSV parses without error");
        auto rows = p.getRows();
        CHECK(rows[0][1] == "New York, NY",  "quoted CSV: embedded comma preserved");
        CHECK(rows[1][1] == "London",        "quoted CSV: unquoted field unchanged");
    }

    // --- escaped double-quote inside quoted field ---
    writeTempCSV("./test_tmp/test_dquote.csv",
        "id,value\n"
        "1,\"say \"\"hello\"\"\"\n"
    );
    {
        CSVParser p("./test_tmp/test_dquote.csv");
        CHECK(p.parse(), "double-quote CSV parses");
        auto rows = p.getRows();
        CHECK(rows[0][1] == "say \"hello\"",  "double-quote: escaped quotes decoded");
    }

    // --- empty fields ---
    writeTempCSV("./test_tmp/test_empty.csv",
        "a,b,c\n"
        "1,,3\n"
        ",2,\n"
    );
    {
        CSVParser p("./test_tmp/test_empty.csv");
        CHECK(p.parse(), "empty-field CSV parses");
        auto rows = p.getRows();
        CHECK(rows[0][1] == "",   "empty field: middle empty is empty string");
        CHECK(rows[1][0] == "",   "empty field: leading empty is empty string");
        CHECK(rows[1][2] == "",   "empty field: trailing empty is empty string");
    }

    // --- Windows-style \r\n line endings ---
    writeTempCSV("./test_tmp/test_crlf.csv",
        "x,y\r\n10,20\r\n30,40\r\n"
    );
    {
        CSVParser p("./test_tmp/test_crlf.csv");
        CHECK(p.parse(), "CRLF CSV parses");
        auto rows = p.getRows();
        CHECK(rows[0][0] == "10" && rows[0][1] == "20", "CRLF: first row correct");
        CHECK(rows[1][0] == "30" && rows[1][1] == "40", "CRLF: second row correct");
    }

    // --- non-existent file returns false ---
    {
        CSVParser p("./test_tmp/this_file_does_not_exist.csv");
        CHECK(!p.parse(), "non-existent file: parse() returns false");
        CHECK(!p.getLastError().empty(), "non-existent file: error message set");
    }

    std::remove("./test_tmp/test_basic.csv");
    std::remove("./test_tmp/test_quoted.csv");
    std::remove("./test_tmp/test_dquote.csv");
    std::remove("./test_tmp/test_empty.csv");
    std::remove("./test_tmp/test_crlf.csv");
}

// ============================================================
// 5 + 6. ColumnWriter + ColumnReader roundtrip tests
// These two components are tested together because a writer
// without a reader (or vice versa) can't be fully verified.
// ============================================================
static void test_writer_reader_int64() {
    SECTION("ColumnWriter + ColumnReader — INT64");

    const std::string dir = "./test_tmp/test_col_int64";
    removeDir(dir);
    std::filesystem::create_directories(dir);

    std::vector<int64_t> values = {0, 1, -1, 42, INT64_MAX, INT64_MIN, 99999};

    // Write
    {
        ColumnWriter w(dir, "num", ColumnType::INT64, Encoding::NONE);
        w.setRowCount(values.size());
        CHECK(w.writeInt64(values), "INT64 writer: writeInt64 returns true");
        CHECK(w.finalize(),         "INT64 writer: finalize returns true");
    }

    // Verify .col file exists (temp file must be renamed)
    {
        std::ifstream f(dir + "/num.col");
        CHECK(f.good(), "INT64: .col file exists after finalize");
        std::ifstream tmp(dir + "/num.col.tmp");
        CHECK(!tmp.good(), "INT64: .col.tmp removed after finalize");
    }

    // Read back
    {
        ColumnReader r;
        CHECK(r.open(dir, "num"),             "INT64 reader: open returns true");
        CHECK(r.isOpen(),                      "INT64 reader: isOpen() true");
        CHECK(r.getType() == ColumnType::INT64,"INT64 reader: type is INT64");
        CHECK(r.getRowCount() == values.size(),"INT64 reader: row_count matches");

        size_t idx = 0;
        while (r.hasNext()) {
            ColumnValue v = r.next();
            CHECK(std::holds_alternative<int64_t>(v),
                  "INT64 reader: value is int64_t variant");
            CHECK(std::get<int64_t>(v) == values[idx],
                  "INT64 reader: value[" + std::to_string(idx) + "] == " +
                  std::to_string(values[idx]));
            idx++;
        }
        CHECK(idx == values.size(), "INT64 reader: read exactly N rows");

        // reset() must allow re-reading from the start
        r.reset();
        CHECK(r.hasNext(), "INT64 reader: hasNext() true after reset");
        ColumnValue first = r.next();
        CHECK(std::get<int64_t>(first) == values[0],
              "INT64 reader: first value correct after reset");

        r.close();
        CHECK(!r.isOpen(), "INT64 reader: isOpen() false after close");
    }

    removeDir(dir);
}

static void test_writer_reader_double() {
    SECTION("ColumnWriter + ColumnReader — DOUBLE");

    const std::string dir = "./test_tmp/test_col_double";
    removeDir(dir);
    std::filesystem::create_directories(dir);

    std::vector<double> values = {0.0, 1.5, -3.14, 1e10, -1e-10, 99.99};

    {
        ColumnWriter w(dir, "price", ColumnType::DOUBLE, Encoding::NONE);
        w.setRowCount(values.size());
        CHECK(w.writeDouble(values), "DOUBLE writer: writeDouble returns true");
        CHECK(w.finalize(),           "DOUBLE writer: finalize returns true");
    }

    {
        ColumnReader r;
        CHECK(r.open(dir, "price"),              "DOUBLE reader: open returns true");
        CHECK(r.getType() == ColumnType::DOUBLE, "DOUBLE reader: type is DOUBLE");
        CHECK(r.getRowCount() == values.size(),  "DOUBLE reader: row_count matches");

        size_t idx = 0;
        while (r.hasNext()) {
            ColumnValue v = r.next();
            CHECK(std::holds_alternative<double>(v),
                  "DOUBLE reader: value is double variant");
            // Exact IEEE 754 comparison is fine here because we wrote and read
            // the same bits with no arithmetic in between.
            CHECK(std::get<double>(v) == values[idx],
                  "DOUBLE reader: value[" + std::to_string(idx) + "] matches");
            idx++;
        }
        CHECK(idx == values.size(), "DOUBLE reader: read exactly N rows");
        r.close();
    }

    removeDir(dir);
}

static void test_writer_reader_string() {
    SECTION("ColumnWriter + ColumnReader — STRING");

    const std::string dir = "./test_tmp/test_col_string";
    removeDir(dir);
    std::filesystem::create_directories(dir);

    // Include edge-cases: empty string, very short, unicode-ish bytes
    std::vector<std::string> values = {
        "hello", "world", "", "United States", "a",
        "with spaces", "with,comma", "123"
    };

    {
        ColumnWriter w(dir, "country", ColumnType::STRING, Encoding::NONE);
        w.setRowCount(values.size());
        CHECK(w.writeString(values), "STRING writer: writeString returns true");
        CHECK(w.finalize(),           "STRING writer: finalize returns true");
    }

    {
        ColumnReader r;
        CHECK(r.open(dir, "country"),            "STRING reader: open returns true");
        CHECK(r.getType() == ColumnType::STRING, "STRING reader: type is STRING");
        CHECK(r.getRowCount() == values.size(),  "STRING reader: row_count matches");

        size_t idx = 0;
        while (r.hasNext()) {
            ColumnValue v = r.next();
            CHECK(std::holds_alternative<std::string>(v),
                  "STRING reader: value is string variant");
            CHECK(std::get<std::string>(v) == values[idx],
                  "STRING reader: value[" + std::to_string(idx) +
                  "] == \"" + values[idx] + "\"");
            idx++;
        }
        CHECK(idx == values.size(), "STRING reader: read exactly N rows");
        r.close();
    }

    removeDir(dir);
}

static void test_writer_reader_crc_corruption() {
    SECTION("ColumnReader — CRC corruption detection");

    const std::string dir = "./test_tmp/test_col_crc";
    removeDir(dir);
    std::filesystem::create_directories(dir);

    // Write a valid column
    {
        ColumnWriter w(dir, "val", ColumnType::INT64, Encoding::NONE);
        w.setRowCount(3);
        CHECK(w.writeInt64({10, 20, 30}), "CRC test: write succeeds");
        CHECK(w.finalize(),               "CRC test: finalize succeeds");
    }

    // Corrupt one byte in the middle of the file
    {
        std::string path = dir + "/val.col";
        std::fstream f(path, std::ios::binary | std::ios::in | std::ios::out);
        f.seekp(20);  // land in the data section
        char bad = 0xFF;
        f.write(&bad, 1);
        f.close();
    }

    // Reader must detect the corruption and refuse to open
    {
        ColumnReader r;
        bool opened = r.open(dir, "val");
        CHECK(!opened, "CRC corruption: open() returns false");
        CHECK(!r.getLastError().empty(), "CRC corruption: error message set");
    }

    removeDir(dir);
}

static void test_writer_reader_single_row() {
    SECTION("ColumnWriter + ColumnReader — single row edge case");

    const std::string dir = "./test_tmp/test_col_single";
    removeDir(dir);
    std::filesystem::create_directories(dir);

    {
        ColumnWriter w(dir, "x", ColumnType::INT64, Encoding::NONE);
        w.setRowCount(1);
        CHECK(w.writeInt64({42}), "single row write succeeds");
        CHECK(w.finalize(),        "single row finalize succeeds");
    }
    {
        ColumnReader r;
        CHECK(r.open(dir, "x"),     "single row: open succeeds");
        CHECK(r.getRowCount() == 1, "single row: row_count == 1");
        CHECK(r.hasNext(),          "single row: hasNext true initially");
        ColumnValue v = r.next();
        CHECK(std::get<int64_t>(v) == 42, "single row: value == 42");
        CHECK(!r.hasNext(),         "single row: hasNext false after reading");
        r.close();
    }

    removeDir(dir);
}

static void test_writer_reader_empty() {
    SECTION("ColumnWriter + ColumnReader — zero rows edge case");

    const std::string dir = "./test_tmp/test_col_empty";
    removeDir(dir);
    std::filesystem::create_directories(dir);

    {
        ColumnWriter w(dir, "e", ColumnType::INT64, Encoding::NONE);
        w.setRowCount(0);
        CHECK(w.writeInt64({}), "zero rows write succeeds");
        CHECK(w.finalize(),      "zero rows finalize succeeds");
    }
    {
        ColumnReader r;
        CHECK(r.open(dir, "e"),     "zero rows: open succeeds");
        CHECK(r.getRowCount() == 0, "zero rows: row_count == 0");
        CHECK(!r.hasNext(),         "zero rows: hasNext false immediately");
        r.close();
    }

    removeDir(dir);
}

// ============================================================
// 7. SchemaManager tests
// ============================================================
static void test_schema_manager() {
    SECTION("SchemaManager (schema.cpp)");

    const std::string dir = "./test_tmp/test_schema";
    removeDir(dir);
    std::filesystem::create_directories(dir);

    // schemaExists returns false before writing
    CHECK(!SchemaManager::schemaExists(dir),
          "schemaExists: false before write");

    // Write a schema with all three types and encodings
    std::vector<SchemaColumn> cols = {
        {"id",      ColumnType::INT64,  Encoding::NONE},
        {"price",   ColumnType::DOUBLE, Encoding::NONE},
        {"country", ColumnType::STRING, Encoding::NONE},
    };
    CHECK(SchemaManager::writeSchema(dir, cols, "test_table"),
          "writeSchema: returns true");

    // Temp file must be gone, final file must exist
    {
        std::ifstream tmp(dir + "/schema.json.tmp");
        CHECK(!tmp.good(), "writeSchema: .tmp file removed after rename");
        std::ifstream final_(dir + "/schema.json");
        CHECK(final_.good(), "writeSchema: schema.json exists");
    }

    CHECK(SchemaManager::schemaExists(dir),
          "schemaExists: true after write");

    // Read back and verify every field
    auto read_cols = SchemaManager::readSchema(dir);
    CHECK(read_cols.size() == 3,             "readSchema: 3 columns");
    CHECK(read_cols[0].name == "id",         "readSchema: col0 name == id");
    CHECK(read_cols[0].type == ColumnType::INT64,  "readSchema: col0 type INT64");
    CHECK(read_cols[0].encoding == Encoding::NONE, "readSchema: col0 encoding NONE");
    CHECK(read_cols[1].name == "price",      "readSchema: col1 name == price");
    CHECK(read_cols[1].type == ColumnType::DOUBLE, "readSchema: col1 type DOUBLE");
    CHECK(read_cols[2].name == "country",    "readSchema: col2 name == country");
    CHECK(read_cols[2].type == ColumnType::STRING, "readSchema: col2 type STRING");

    // Overwrite with a different schema — should replace cleanly
    std::vector<SchemaColumn> cols2 = {
        {"x", ColumnType::INT64, Encoding::NONE},
    };
    CHECK(SchemaManager::writeSchema(dir, cols2, "test_table"),
          "writeSchema: overwrite returns true");
    auto read2 = SchemaManager::readSchema(dir);
    CHECK(read2.size() == 1,          "readSchema after overwrite: 1 column");
    CHECK(read2[0].name == "x",       "readSchema after overwrite: name == x");

    // Read from non-existent directory returns empty
    auto empty = SchemaManager::readSchema("./test_tmp/does_not_exist_xyz");
    CHECK(empty.empty(), "readSchema non-existent dir: returns empty vector");

    removeDir(dir);
}

// ============================================================
// 8. QueryParser tests
// ============================================================
static void test_query_parser() {
    SECTION("QueryParser (query_parser.cpp)");

    auto parse = [](const std::string& q, QueryPlan& plan) -> bool {
        QueryParser p(q);
        return p.parse(plan);
    };

    // --- SELECT COUNT(*) FROM t ---
    {
        QueryPlan plan;
        CHECK(parse("SELECT COUNT(*) FROM sales", plan), "parse COUNT(*) FROM");
        CHECK(plan.table == "sales",              "COUNT(*): table == sales");
        CHECK(plan.selects.size() == 1,           "COUNT(*): 1 select expr");
        CHECK(plan.selects[0].agg == AggFunc::COUNT, "COUNT(*): agg == COUNT");
        CHECK(plan.selects[0].star == true,       "COUNT(*): star == true");
        CHECK(!plan.where.has_where,              "COUNT(*): no WHERE");
        CHECK(plan.groupby.empty(),               "COUNT(*): no GROUP BY");
    }

    // --- SELECT SUM(price) FROM sales WHERE date >= 20240101 ---
    {
        QueryPlan plan;
        CHECK(parse("SELECT SUM(price) FROM sales WHERE date >= 20240101", plan),
              "parse SUM with WHERE");
        CHECK(plan.table == "sales",               "SUM WHERE: table");
        CHECK(plan.selects[0].agg == AggFunc::SUM, "SUM WHERE: agg == SUM");
        CHECK(plan.selects[0].col == "price",      "SUM WHERE: col == price");
        CHECK(plan.where.has_where,                "SUM WHERE: has_where true");
        CHECK(plan.where.col == "date",            "SUM WHERE: where col == date");
        CHECK(plan.where.op  == ">=",              "SUM WHERE: where op == >=");
        CHECK(plan.where.val == "20240101",        "SUM WHERE: where val");
    }

    // --- SELECT * FROM t ---
    {
        QueryPlan plan;
        CHECK(parse("SELECT * FROM inventory", plan), "parse SELECT *");
        CHECK(plan.select_star,             "SELECT *: select_star true");
        CHECK(plan.table == "inventory",    "SELECT *: table == inventory");
    }

    // --- SELECT col1, col2 FROM t ---
    {
        QueryPlan plan;
        CHECK(parse("SELECT id, name FROM users", plan), "parse multi-column SELECT");
        CHECK(plan.selects.size() == 2,         "multi-col: 2 select exprs");
        CHECK(plan.selects[0].col == "id",       "multi-col: col0 == id");
        CHECK(plan.selects[1].col == "name",     "multi-col: col1 == name");
        CHECK(plan.selects[0].agg == AggFunc::NONE, "multi-col: no agg on col0");
    }

    // --- GROUP BY ---
    {
        QueryPlan plan;
        CHECK(parse("SELECT country, SUM(price) FROM sales GROUP BY country", plan),
              "parse GROUP BY");
        CHECK(plan.groupby == "country", "GROUP BY: groupby == country");
    }

    // --- All comparison operators ---
    const std::vector<std::string> ops = {"=", "!=", "<", "<=", ">", ">="};
    for (const auto& op : ops) {
        QueryPlan plan;
        std::string q = "SELECT x FROM t WHERE y " + op + " 5";
        CHECK(parse(q, plan) && plan.where.op == op,
              "WHERE operator: " + op);
    }

    // --- String literal in WHERE ---
    {
        QueryPlan plan;
        CHECK(parse("SELECT x FROM t WHERE cat = 'Electronics'", plan),
              "parse string literal WHERE");
        CHECK(plan.where.val == "Electronics",
              "string literal: val == Electronics (no quotes)");
    }

    // --- AVG, MIN, MAX ---
    {
        QueryPlan plan;
        CHECK(parse("SELECT AVG(qty), MIN(price), MAX(price) FROM t", plan),
              "parse AVG MIN MAX");
        CHECK(plan.selects[0].agg == AggFunc::AVG, "AVG parsed");
        CHECK(plan.selects[1].agg == AggFunc::MIN, "MIN parsed");
        CHECK(plan.selects[2].agg == AggFunc::MAX, "MAX parsed");
    }

    // --- case insensitivity ---
    {
        QueryPlan plan;
        CHECK(parse("select sum(price) from sales where date >= 1", plan),
              "parse lowercase keywords");
        CHECK(plan.table == "sales",               "lowercase: table correct");
        CHECK(plan.selects[0].agg == AggFunc::SUM, "lowercase: SUM parsed");
    }

    // --- neededColumns() ---
    {
        QueryPlan plan;
        parse("SELECT country, SUM(price) FROM sales WHERE date >= 1", plan);
        auto needed = plan.neededColumns();
        // Should contain country, price, date — no duplicates
        CHECK(needed.size() == 3, "neededColumns: 3 unique columns");
        bool has_country = false, has_price = false, has_date = false;
        for (const auto& c : needed) {
            if (c == "country") has_country = true;
            if (c == "price")   has_price   = true;
            if (c == "date")    has_date    = true;
        }
        CHECK(has_country && has_price && has_date,
              "neededColumns: country, price, date all present");
    }

    // --- neededColumns with repeated column ---
    {
        QueryPlan plan;
        parse("SELECT price FROM t WHERE price > 5", plan);
        auto needed = plan.neededColumns();
        // price appears in both SELECT and WHERE — must not be duplicated
        CHECK(needed.size() == 1, "neededColumns: no duplicates when same col in SELECT+WHERE");
    }

    // --- Syntax errors ---
    {
        QueryPlan plan;
        QueryParser p1("FROM t");
        CHECK(!p1.parse(plan), "parse error: missing SELECT");

        QueryParser p2("SELECT x");
        CHECK(!p2.parse(plan), "parse error: missing FROM");

        QueryParser p3("");
        CHECK(!p3.parse(plan), "parse error: empty query");
    }
}

// ============================================================
// 9. PredicateEvaluator tests
// ============================================================
static void test_predicate_evaluator() {
    SECTION("PredicateEvaluator (predicate.cpp)");

    // Set up a small INT64 column: [10, 20, 30, 40, 50]
    const std::string dir = "./test_tmp/test_pred";
    removeDir(dir);
    std::filesystem::create_directories(dir);

    {
        ColumnWriter w(dir, "val", ColumnType::INT64, Encoding::NONE);
        w.setRowCount(5);
        w.writeInt64({10, 20, 30, 40, 50});
        w.finalize();
    }

    auto eval = [&](const std::string& op, const std::string& literal) -> Bitmap {
        WherePredicate pred;
        pred.has_where = true;
        pred.col = "val";
        pred.op  = op;
        pred.val = literal;
        return PredicateEvaluator::evaluate(pred, dir, 5);
    };

    // = 30 -> only row 2 passes
    {
        auto bm = eval("=", "30");
        CHECK(bm.size() == 5,  "predicate =: bitmap size 5");
        CHECK(!bm[0] && !bm[1] && bm[2] && !bm[3] && !bm[4],
              "predicate =: only row 2 passes");
        CHECK(PredicateEvaluator::countTrue(bm) == 1, "predicate =: countTrue == 1");
    }

    // != 30 -> rows 0,1,3,4 pass
    {
        auto bm = eval("!=", "30");
        CHECK(PredicateEvaluator::countTrue(bm) == 4, "predicate !=: 4 rows pass");
        CHECK(!bm[2], "predicate !=: row 2 excluded");
    }

    // < 30 -> rows 0,1 pass
    {
        auto bm = eval("<", "30");
        CHECK(PredicateEvaluator::countTrue(bm) == 2, "predicate <: 2 rows pass");
        CHECK(bm[0] && bm[1] && !bm[2], "predicate <: rows 0,1 pass, row 2 excluded");
    }

    // <= 30 -> rows 0,1,2 pass
    {
        auto bm = eval("<=", "30");
        CHECK(PredicateEvaluator::countTrue(bm) == 3, "predicate <=: 3 rows pass");
    }

    // > 30 -> rows 3,4 pass
    {
        auto bm = eval(">", "30");
        CHECK(PredicateEvaluator::countTrue(bm) == 2, "predicate >: 2 rows pass");
        CHECK(bm[3] && bm[4], "predicate >: rows 3,4 pass");
    }

    // >= 30 -> rows 2,3,4 pass
    {
        auto bm = eval(">=", "30");
        CHECK(PredicateEvaluator::countTrue(bm) == 3, "predicate >=: 3 rows pass");
    }

    // No WHERE -> all rows pass
    {
        WherePredicate no_where;
        no_where.has_where = false;
        auto bm = PredicateEvaluator::evaluate(no_where, dir, 5);
        CHECK(PredicateEvaluator::countTrue(bm) == 5, "no WHERE: all 5 rows pass");
    }

    // Value that matches no rows -> all false
    {
        auto bm = eval("=", "999");
        CHECK(PredicateEvaluator::countTrue(bm) == 0, "predicate = 999: 0 rows pass");
    }

    // String column predicate
    {
        ColumnWriter w(dir, "cat", ColumnType::STRING, Encoding::NONE);
        w.setRowCount(4);
        w.writeString({"Electronics", "Books", "Electronics", "Clothing"});
        w.finalize();

        WherePredicate pred;
        pred.has_where = true;
        pred.col = "cat";
        pred.op  = "=";
        pred.val = "Electronics";
        auto bm = PredicateEvaluator::evaluate(pred, dir, 4);
        CHECK(PredicateEvaluator::countTrue(bm) == 2, "string predicate =: 2 matches");
        CHECK(bm[0] && !bm[1] && bm[2] && !bm[3],    "string predicate =: rows 0,2 pass");
    }

    // evaluateWithReader — reader must be reset after evaluation
    {
        ColumnReader r;
        r.open(dir, "val");
        WherePredicate pred;
        pred.has_where = true;
        pred.col = "val";
        pred.op  = ">";
        pred.val = "25";
        auto bm = PredicateEvaluator::evaluateWithReader(pred, r);
        CHECK(bm.size() == 5, "evaluateWithReader: bitmap size correct");
        // After evaluateWithReader, reader must be reset so the caller can reuse it
        CHECK(r.hasNext(), "evaluateWithReader: reader is reset after call");
        r.close();
    }

    // countTrue on all-true and all-false bitmaps
    {
        Bitmap all_true(10, true);
        Bitmap all_false(10, false);
        CHECK(PredicateEvaluator::countTrue(all_true)  == 10, "countTrue all-true: 10");
        CHECK(PredicateEvaluator::countTrue(all_false) == 0,  "countTrue all-false: 0");
        CHECK(PredicateEvaluator::countTrue({})        == 0,  "countTrue empty: 0");
    }

    removeDir(dir);
}

// ============================================================
// 10. End-to-end roundtrip tests
// Write a multi-column table, read it back, run predicates.
// This is the closest thing to an integration test without
// spinning up the full REPL shell.
// ============================================================
static void test_end_to_end() {
    SECTION("End-to-end: multi-column write + read + predicate");

    const std::string dir = "./test_tmp/test_e2e";
    removeDir(dir);
    std::filesystem::create_directories(dir);

    // Simulate a tiny "sales" table:
    //   id     int64:  [1, 2, 3, 4, 5]
    //   price  double: [9.99, 24.99, 4.99, 49.99, 14.99]
    //   cat    string: [Books, Electronics, Books, Electronics, Clothing]

    std::vector<int64_t>    ids    = {1, 2, 3, 4, 5};
    std::vector<double>     prices = {9.99, 24.99, 4.99, 49.99, 14.99};
    std::vector<std::string> cats  = {"Books","Electronics","Books","Electronics","Clothing"};

    {
        ColumnWriter wi(dir, "id",    ColumnType::INT64,  Encoding::NONE);
        wi.setRowCount(5); wi.writeInt64(ids);   wi.finalize();

        ColumnWriter wp(dir, "price", ColumnType::DOUBLE, Encoding::NONE);
        wp.setRowCount(5); wp.writeDouble(prices); wp.finalize();

        ColumnWriter wc(dir, "cat",   ColumnType::STRING, Encoding::NONE);
        wc.setRowCount(5); wc.writeString(cats);  wc.finalize();

        std::vector<SchemaColumn> schema = {
            {"id",    ColumnType::INT64,  Encoding::NONE},
            {"price", ColumnType::DOUBLE, Encoding::NONE},
            {"cat",   ColumnType::STRING, Encoding::NONE},
        };
        CHECK(SchemaManager::writeSchema(dir, schema, "sales"),
              "e2e: schema written");
    }

    // Read schema back
    auto schema = SchemaManager::readSchema(dir);
    CHECK(schema.size() == 3, "e2e: schema has 3 columns");

    // Open all three readers
    ColumnReader r_id, r_price, r_cat;
    CHECK(r_id.open(dir, "id"),       "e2e: id reader opens");
    CHECK(r_price.open(dir, "price"), "e2e: price reader opens");
    CHECK(r_cat.open(dir, "cat"),     "e2e: cat reader opens");

    // Predicate: price > 10.0  ->  rows 1, 3, 4 pass (prices 24.99, 49.99, 14.99)
    WherePredicate pred;
    pred.has_where = true;
    pred.col = "price";
    pred.op  = ">";
    pred.val = "10.0";
    auto bitmap = PredicateEvaluator::evaluateWithReader(pred, r_price);

    CHECK(bitmap.size() == 5,  "e2e predicate: bitmap size 5");
    CHECK(!bitmap[0],          "e2e predicate: row 0 (9.99) excluded");
    CHECK(bitmap[1],           "e2e predicate: row 1 (24.99) included");
    CHECK(!bitmap[2],          "e2e predicate: row 2 (4.99) excluded");
    CHECK(bitmap[3],           "e2e predicate: row 3 (49.99) included");
    CHECK(bitmap[4],           "e2e predicate: row 4 (14.99) included");
    CHECK(PredicateEvaluator::countTrue(bitmap) == 3, "e2e predicate: 3 rows pass");

    // Manual SUM(price) WHERE price > 10.0
    // Scan price column using the bitmap (simulating the executor)
    double sum = 0.0;
    size_t row = 0;
    while (r_price.hasNext()) {
        ColumnValue v = r_price.next();
        if (bitmap[row]) sum += std::get<double>(v);
        row++;
    }
    // 24.99 + 49.99 + 14.99 = 89.97
    CHECK(std::abs(sum - 89.97) < 0.001, "e2e SUM(price) WHERE price > 10 = 89.97");

    // Manual collect of category names for passing rows
    std::vector<std::string> passing_cats;
    row = 0;
    while (r_cat.hasNext()) {
        ColumnValue v = r_cat.next();
        if (bitmap[row]) passing_cats.push_back(std::get<std::string>(v));
        row++;
    }
    CHECK(passing_cats.size() == 3,          "e2e: 3 categories for passing rows");
    CHECK(passing_cats[0] == "Electronics",  "e2e: passing cat[0] == Electronics");
    CHECK(passing_cats[1] == "Electronics",  "e2e: passing cat[1] == Electronics");
    CHECK(passing_cats[2] == "Clothing",     "e2e: passing cat[2] == Clothing");

    r_id.close(); r_price.close(); r_cat.close();

    // End-to-end CSV roundtrip via the CSVParser + TypeInference pipeline
    writeTempCSV(dir + "/mini.csv",
        "id,value,label\n"
        "10,1.5,alpha\n"
        "20,2.5,beta\n"
        "30,3.5,gamma\n"
    );
    {
        CSVParser p(dir + "/mini.csv");
        CHECK(p.parse(), "e2e CSV roundtrip: parse succeeds");
        auto rows = p.getRows();
        auto types = TypeInference::inferAllTypes(rows, 3);
        CHECK(types[0] == ColumnType::INT64,  "e2e CSV roundtrip: col0 inferred INT64");
        CHECK(types[1] == ColumnType::DOUBLE, "e2e CSV roundtrip: col1 inferred DOUBLE");
        CHECK(types[2] == ColumnType::STRING, "e2e CSV roundtrip: col2 inferred STRING");
    }

    removeDir(dir);
}

// ============================================================
// main — runs all sections and prints a final summary
// ============================================================
int main() {
    // Create ./test_tmp/ base directory for all test files.
    // This replaces /tmp/ which does not exist on Windows.
    ensureBaseDir();

    std::cout << "============================================\n";
    std::cout << "  Phase 1 Test Suite\n";
    std::cout << "============================================\n";

    test_crc64();
    test_type_converters();
    test_type_inference();
    test_csv_parser();
    test_writer_reader_int64();
    test_writer_reader_double();
    test_writer_reader_string();
    test_writer_reader_crc_corruption();
    test_writer_reader_single_row();
    test_writer_reader_empty();
    test_schema_manager();
    test_query_parser();
    test_predicate_evaluator();
    test_end_to_end();

    // -- Summary ------------------------------------------
    std::cout << "\n============================================\n";
    std::cout << "  Results: " << g_passed << " passed, "
              << g_failed << " failed\n";
    std::cout << "============================================\n";

    // Return non-zero exit code if any test failed — useful for CI / VS Code
    // tasks that check the process exit code.
    return g_failed == 0 ? 0 : 1;
}

// ============================================================
// HOW TO SET UP AND RUN IN VS CODE — STEP BY STEP
// ============================================================
//
// PREREQUISITES
// -------------
// 1. Install the "C/C++" extension by Microsoft (Extension ID: ms-vscode.cpptools).
// 2. Make sure g++ is installed:
//      Linux:   sudo apt install g++
//      macOS:   xcode-select --install
//      Windows: install MinGW-w64 or use WSL
//
// YOUR PROJECT LAYOUT (assumed)
// ------------------------------
//   project_root/
//     include/
//       common.h
//       type_inference.h
//       csv_parser.h
//       column_writer.h
//       column_reader.h
//       schema.h
//       query_parser.h
//       predicate.h
//     src/
//       common.cpp
//       type_inference.cpp
//       csv_parser.cpp
//       column_writer.cpp
//       column_reader.cpp
//       schema.cpp
//       query_parser.cpp
//       predicate.cpp
//     test_phase1.cpp    ← this file
//
// OPTION A — TERMINAL INSIDE VS CODE (easiest)
// --------------------------------------------
// 1. Open VS Code in your project root (File -> Open Folder).
// 2. Open the integrated terminal:  Ctrl+` (backtick)  or  View -> Terminal.
// 3. Paste and run this command:
//
//      g++ -std=c++17 -Wall -o test_phase1 \
//          test_phase1.cpp \
//          src/common.cpp \
//          src/type_inference.cpp \
//          src/csv_parser.cpp \
//          src/column_writer.cpp \
//          src/column_reader.cpp \
//          src/schema.cpp \
//          src/query_parser.cpp \
//          src/predicate.cpp \
//          && ./test_phase1
//
//    On Windows (MinGW):
//      g++ -std=c++17 -Wall -o test_phase1.exe ^
//          test_phase1.cpp ^
//          src/common.cpp ^
//          ...same files... ^
//          && test_phase1.exe
//
// OPTION B — tasks.json (build shortcut Ctrl+Shift+B)
// ----------------------------------------------------
// 1. In VS Code press Ctrl+Shift+P -> "Tasks: Configure Default Build Task"
//    -> "Create tasks.json file from template" -> "Others".
// 2. Replace the contents of .vscode/tasks.json with:
//
//    {
//      "version": "2.0.0",
//      "tasks": [
//        {
//          "label": "Build & Run Tests",
//          "type": "shell",
//          "command": "g++",
//          "args": [
//            "-std=c++17", "-Wall", "-g",
//            "-o", "test_phase1",
//            "test_phase1.cpp",
//            "src/common.cpp",
//            "src/type_inference.cpp",
//            "src/csv_parser.cpp",
//            "src/column_writer.cpp",
//            "src/column_reader.cpp",
//            "src/schema.cpp",
//            "src/query_parser.cpp",
//            "src/predicate.cpp",
//            "&&", "./test_phase1"
//          ],
//          "group": { "kind": "build", "isDefault": true },
//          "problemMatcher": "$gcc",
//          "detail": "Compile and run Phase 1 tests"
//        }
//      ]
//    }
//
// 3. Press Ctrl+Shift+B to build and run. Output appears in the Terminal panel.
//
// OPTION C — launch.json (run with the debugger, F5)
// ---------------------------------------------------
// 1. First compile with -g (debug symbols):
//      g++ -std=c++17 -g -o test_phase1 test_phase1.cpp src/*.cpp
// 2. Press Ctrl+Shift+P -> "Debug: Open launch.json".
// 3. Add this configuration inside the "configurations" array:
//
//    {
//      "name": "Debug Tests",
//      "type": "cppdbg",
//      "request": "launch",
//      "program": "${workspaceFolder}/test_phase1",
//      "args": [],
//      "stopAtEntry": false,
//      "cwd": "${workspaceFolder}",
//      "environment": [],
//      "externalConsole": false,
//      "MIMode": "gdb",        // use "lldb" on macOS
//      "setupCommands": [
//        {
//          "description": "Enable pretty-printing",
//          "text": "-enable-pretty-printing",
//          "ignoreFailures": true
//        }
//      ]
//    }
//
// 4. Set breakpoints by clicking the gutter next to any line number.
// 5. Press F5 to start debugging. The test output appears in the Debug Console.
//
// READING THE OUTPUT
// ------------------
// Each line looks like one of:
//   [PASS] CRC64 is deterministic for same input
//   [FAIL] CRC64 of empty buffer is 0
//
// A [FAIL] means the condition in CHECK() was false. Read the description
// to know which test failed, then look at the corresponding test_*() function
// in this file to see exactly what was being verified.
//
// The final summary line shows totals:
//   Results: 87 passed, 0 failed
//
// The process exits with code 0 if all tests pass, 1 if any fail.
// This means you can use it in a CI script:
//   ./test_phase1 || echo "Tests failed!"
// ============================================================