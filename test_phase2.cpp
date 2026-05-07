// ============================================================
// test_phase2.cpp — Comprehensive Phase 1 + Phase 2 Test Suite
//
// Covers every component required by manual Sections 8 and 9:
//
//  PHASE 1 (unchanged from test_phase1.cpp):
//   1.  CRC64                   (common.cpp)
//   2.  Type converters         (common.cpp)
//   3.  TypeInference           (type_inference.cpp)
//   4.  CSVParser               (csv_parser.cpp)
//   5.  ColumnWriter NONE       (column_writer.cpp)
//   6.  ColumnReader NONE       (column_reader.cpp)
//   7.  SchemaManager           (schema.cpp)
//   8.  QueryParser             (query_parser.cpp)
//   9.  PredicateEvaluator      (predicate.cpp)
//  10.  End-to-end Phase 1      (write + read + predicate)
//
//  PHASE 2 (new):
//  11.  ColumnWriter DICTIONARY + ColumnReader DICTIONARY round-trip
//  12.  ColumnWriter RLE        + ColumnReader RLE        round-trip
//  13.  Schema round-trip with DICTIONARY and RLE encodings
//  14.  Encoding-selection logic (loader chooses the right encoding)
//  15.  GROUP BY executor path  (end-to-end with dict column)
//  16.  Edge cases: single-distinct-value dict, all-same RLE, large runs
//
// HOW TO COMPILE:
//   g++ -std=c++17 -Wall -o test_phase2 \
//       test_phase2.cpp \
//       src/common.cpp src/type_inference.cpp src/csv_parser.cpp \
//       src/column_writer.cpp src/column_reader.cpp src/schema.cpp \
//       src/query_parser.cpp src/predicate.cpp src/loader.cpp \
//       src/executor.cpp \
//       && ./test_phase2
//
// (loader.cpp and executor.cpp are needed for Sections 14 and 15.
//  If they are not yet implemented, comment out test_encoding_selection()
//  and test_group_by_executor() and recompile without those two .cpp files.)
// ============================================================

#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <variant>
#include <cassert>
#include <cstdio>
#include <cmath>
#include <filesystem>
#include <unordered_map>
#include <algorithm>

#include "include/common.h"
#include "include/type_inference.h"
#include "include/csv_parser.h"
#include "include/column_writer.h"
#include "include/column_reader.h"
#include "include/schema.h"
#include "include/query_parser.h"
#include "include/predicate.h"
#include "include/loader.h"
#include "include/executor.h"

// ============================================================
// Tiny test harness
// ============================================================
static int g_passed = 0;
static int g_failed = 0;

static void CHECK(bool condition, const std::string& description) {
    if (condition) {
        std::cout << "  [PASS] " << description << "\n";
        g_passed++;
    } else {
        std::cout << "  [FAIL] " << description << "\n";
        g_failed++;
    }
}

static void SECTION(const std::string& name) {
    std::cout << "\n-- " << name << " --\n";
}

// ============================================================
// Shared helpers
// ============================================================
static void writeTempCSV(const std::string& path, const std::string& content) {
    std::ofstream f(path);
    f << content;
}

static void removeDir(const std::string& path) {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
}

static void ensureBaseDir() {
    std::error_code ec;
    std::filesystem::create_directories("./test_tmp", ec);
}

// ============================================================
// Helper: write a DICTIONARY-encoded column file manually.
// Used by Phase 2 reader tests so we can test the reader
// independently of the writer.
//
// Format written (matches manual Section 4 + Section 5.1):
//   Header (36 bytes)
//   Data section: N uint8/uint16 ids (depending on D)
//   Dictionary section: D entries, each = uint16 len + bytes
//   CRC64 footer (8 bytes)
//
// This helper always uses 1-byte ids (D <= 256), which is the
// common case for country/category columns.
// ============================================================
static bool writeDictColumn(const std::string& dir,
                            const std::string& name,
                            const std::vector<std::string>& values) {
    // Build dictionary
    std::vector<std::string> dict;
    std::unordered_map<std::string, uint8_t> id_map;
    for (const auto& v : values) {
        if (id_map.find(v) == id_map.end()) {
            id_map[v] = static_cast<uint8_t>(dict.size());
            dict.push_back(v);
        }
    }

    // Build data section: N uint8 ids
    std::vector<uint8_t> ids;
    for (const auto& v : values) ids.push_back(id_map[v]);

    // Build dictionary section
    std::vector<char> dict_bytes;
    for (const auto& s : dict) {
        uint16_t len = static_cast<uint16_t>(s.size());
        const char* lp = reinterpret_cast<const char*>(&len);
        dict_bytes.push_back(lp[0]);
        dict_bytes.push_back(lp[1]);
        for (char c : s) dict_bytes.push_back(c);
    }

    uint64_t data_size = ids.size();       // 1 byte per id
    uint64_t dict_size = dict_bytes.size();

    // Build header
    ColumnHeader hdr;
    hdr.magic     = MAGIC;
    hdr.version   = VERSION;
    hdr.type      = static_cast<uint8_t>(ColumnType::STRING);
    hdr.encoding  = static_cast<uint8_t>(Encoding::DICTIONARY);
    hdr.reserved  = 0;
    hdr.row_count = values.size();
    hdr.data_size = data_size;
    hdr.dict_size = dict_size;

    std::string temp = dir + "/" + name + ".col.tmp";
    std::string final = dir + "/" + name + ".col";

    std::ofstream f(temp, std::ios::binary);
    if (!f.is_open()) return false;

    f.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
    f.write(reinterpret_cast<const char*>(ids.data()), ids.size());
    f.write(dict_bytes.data(), dict_bytes.size());
    f.close();

    // Compute and append CRC
    std::ifstream rb(temp, std::ios::binary);
    std::vector<char> buf((std::istreambuf_iterator<char>(rb)),
                           std::istreambuf_iterator<char>());
    rb.close();
    uint64_t crc = crc64(buf.data(), buf.size());
    std::ofstream app(temp, std::ios::binary | std::ios::app);
    app.write(reinterpret_cast<const char*>(&crc), sizeof(crc));
    app.close();

    return std::rename(temp.c_str(), final.c_str()) == 0;
}

// ============================================================
// Helper: write an RLE-encoded INT64 column file manually.
// Format: (value:int64, run_length:uint64) pairs.
// ============================================================
static bool writeRLEColumn(const std::string& dir,
                           const std::string& name,
                           const std::vector<int64_t>& values) {
    // Compute runs
    struct Run { int64_t val; uint64_t len; };
    std::vector<Run> runs;
    if (!values.empty()) {
        runs.push_back({values[0], 1});
        for (size_t i = 1; i < values.size(); i++) {
            if (values[i] == runs.back().val)
                runs.back().len++;
            else
                runs.push_back({values[i], 1});
        }
    }

    uint64_t data_size = runs.size() * (sizeof(int64_t) + sizeof(uint64_t));

    ColumnHeader hdr;
    hdr.magic     = MAGIC;
    hdr.version   = VERSION;
    hdr.type      = static_cast<uint8_t>(ColumnType::INT64);
    hdr.encoding  = static_cast<uint8_t>(Encoding::RLE);
    hdr.reserved  = 0;
    hdr.row_count = values.size();
    hdr.data_size = data_size;
    hdr.dict_size = 0;

    std::string temp = dir + "/" + name + ".col.tmp";
    std::string final = dir + "/" + name + ".col";

    std::ofstream f(temp, std::ios::binary);
    if (!f.is_open()) return false;
    f.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
    for (const auto& r : runs) {
        f.write(reinterpret_cast<const char*>(&r.val), sizeof(int64_t));
        f.write(reinterpret_cast<const char*>(&r.len), sizeof(uint64_t));
    }
    f.close();

    std::ifstream rb(temp, std::ios::binary);
    std::vector<char> buf((std::istreambuf_iterator<char>(rb)),
                           std::istreambuf_iterator<char>());
    rb.close();
    uint64_t crc = crc64(buf.data(), buf.size());
    std::ofstream app(temp, std::ios::binary | std::ios::app);
    app.write(reinterpret_cast<const char*>(&crc), sizeof(crc));
    app.close();

    return std::rename(temp.c_str(), final.c_str()) == 0;
}

// ============================================================
// ========== PHASE 1 TESTS (unchanged) =======================
// ============================================================

// ── 1. CRC64 ─────────────────────────────────────────────────
static void test_crc64() {
    SECTION("CRC64 (common.cpp)");

    uint64_t empty_crc = crc64(nullptr, 0);
    CHECK(empty_crc == 0, "CRC64 of empty buffer is 0");

    const char* data = "hello world";
    uint64_t crc_a = crc64(data, 11);
    uint64_t crc_b = crc64(data, 11);
    CHECK(crc_a == crc_b, "CRC64 is deterministic for same input");

    uint64_t crc_c = crc64("hello World", 11);
    CHECK(crc_a != crc_c, "CRC64 differs for different input");

    uint64_t incremental = 0;
    incremental = crc64_update(incremental, "hello", 5);
    incremental = crc64_update(incremental, " world", 6);
    CHECK(incremental == crc_a, "CRC64 incremental update matches one-shot");

    const char one = 0x42;
    CHECK(crc64(&one, 1) != 0, "CRC64 of single non-zero byte is non-zero");
}

// ── 2. Type converters ────────────────────────────────────────
static void test_type_converters() {
    SECTION("Type / Encoding converters (common.cpp)");

    CHECK(std::string(typeToString(ColumnType::INT32))  == "int32",  "typeToString INT32");
    CHECK(std::string(typeToString(ColumnType::INT64))  == "int64",  "typeToString INT64");
    CHECK(std::string(typeToString(ColumnType::DOUBLE)) == "double", "typeToString DOUBLE");
    CHECK(std::string(typeToString(ColumnType::STRING)) == "string", "typeToString STRING");

    CHECK(stringToType("int32")  == ColumnType::INT32,  "stringToType int32");
    CHECK(stringToType("int64")  == ColumnType::INT64,  "stringToType int64");
    CHECK(stringToType("double") == ColumnType::DOUBLE, "stringToType double");
    CHECK(stringToType("string") == ColumnType::STRING, "stringToType string");
    CHECK(stringToType("banana") == ColumnType::STRING, "stringToType unknown -> STRING");

    CHECK(std::string(encodingToString(Encoding::NONE))       == "none",       "encodingToString NONE");
    CHECK(std::string(encodingToString(Encoding::DICTIONARY)) == "dictionary", "encodingToString DICTIONARY");
    CHECK(std::string(encodingToString(Encoding::RLE))        == "rle",        "encodingToString RLE");

    CHECK(stringToEncoding("none")       == Encoding::NONE,       "stringToEncoding none");
    CHECK(stringToEncoding("dictionary") == Encoding::DICTIONARY, "stringToEncoding dictionary");
    CHECK(stringToEncoding("rle")        == Encoding::RLE,        "stringToEncoding rle");
    CHECK(stringToEncoding("unknown")    == Encoding::NONE,       "stringToEncoding unknown -> NONE");
}

// ── 3. TypeInference ─────────────────────────────────────────
static void test_type_inference() {
    SECTION("TypeInference (type_inference.cpp)");

    CHECK(TypeInference::isInt64("0"),   "isInt64: 0");
    CHECK(TypeInference::isInt64("42"),  "isInt64: 42");
    CHECK(TypeInference::isInt64("-999"),"isInt64: -999");
    CHECK(TypeInference::isInt64("9223372036854775807"), "isInt64: INT64_MAX");

    CHECK(!TypeInference::isInt64(""),    "isInt64: empty -> false");
    CHECK(!TypeInference::isInt64("3.14"),"isInt64: float -> false");
    CHECK(!TypeInference::isInt64("abc"), "isInt64: letters -> false");
    CHECK(!TypeInference::isInt64("1e5"), "isInt64: scientific -> false");

    CHECK(TypeInference::isDouble("3.14"), "isDouble: 3.14");
    CHECK(TypeInference::isDouble("1e5"),  "isDouble: 1e5");
    CHECK(TypeInference::isDouble("-0.001"),"isDouble: -0.001");
    CHECK(TypeInference::isDouble("42"),   "isDouble: integer is valid double");

    CHECK(!TypeInference::isDouble(""),   "isDouble: empty -> false");
    CHECK(!TypeInference::isDouble("abc"),"isDouble: letters -> false");

    // FIXED: INT32 is now the default for small integers
    CHECK(TypeInference::inferType({"1","2","3"}) == ColumnType::INT32,
          "inferType: all integers -> INT32");
    CHECK(TypeInference::inferType({"1","2.5","3"}) == ColumnType::DOUBLE,
          "inferType: int+float mix -> DOUBLE");
    CHECK(TypeInference::inferType({"1","hello","3"}) == ColumnType::STRING,
          "inferType: has string -> STRING");
    CHECK(TypeInference::inferType({"99.9","","14.5"}) == ColumnType::DOUBLE,
          "inferType: empty cells skipped, still DOUBLE");
    // FIXED: small ints with empty cells -> INT32
    CHECK(TypeInference::inferType({"10","","20"}) == ColumnType::INT32,
          "inferType: empty cells skipped, still INT32");
    // FIXED: all empty -> INT32 (all_int flag never cleared, min/max at INT32 bounds)
     CHECK(TypeInference::inferType({"","",""}) == ColumnType::STRING,
          "inferType: all empty cells -> STRING");
    CHECK(TypeInference::inferType({}) == ColumnType::STRING,
          "inferType: empty vector -> STRING");

    std::vector<Row> rows = {{"1","3.14","hello"},{"2","2.71","world"}};
    auto types = TypeInference::inferAllTypes(rows, 3);
    // FIXED: col0 values "1","2" fit in INT32
    CHECK(types[0] == ColumnType::INT32,  "inferAllTypes: col0 -> INT32");
    CHECK(types[1] == ColumnType::DOUBLE, "inferAllTypes: col1 -> DOUBLE");
    CHECK(types[2] == ColumnType::STRING, "inferAllTypes: col2 -> STRING");
}

// ── 4. CSVParser ─────────────────────────────────────────────
static void test_csv_parser() {
    SECTION("CSVParser (csv_parser.cpp)");

    writeTempCSV("./test_tmp/basic.csv",
        "id,name,price\n1,Alice,9.99\n2,Bob,14.50\n3,Charlie,4.00\n");
    {
        CSVParser p("./test_tmp/basic.csv");
        CHECK(p.parse(), "basic CSV: parse ok");
        auto h = p.getHeaders();
        CHECK(h.size()==3 && h[0]=="id" && h[1]=="name" && h[2]=="price",
              "basic CSV: headers correct");
        CHECK(p.getRowCount()==3, "basic CSV: 3 rows");
        auto rows = p.getRows();
        CHECK(rows[0][0]=="1" && rows[1][1]=="Bob" && rows[2][2]=="4.00",
              "basic CSV: spot-check values");
    }

    writeTempCSV("./test_tmp/quoted.csv",
        "id,city,note\n1,\"New York, NY\",ok\n2,London,fine\n");
    {
        CSVParser p("./test_tmp/quoted.csv");
        CHECK(p.parse(), "quoted CSV: parse ok");
        auto rows = p.getRows();
        CHECK(rows[0][1]=="New York, NY", "quoted CSV: embedded comma preserved");
        CHECK(rows[1][1]=="London",       "quoted CSV: unquoted field unchanged");
    }

    writeTempCSV("./test_tmp/dquote.csv", "id,value\n1,\"say \"\"hello\"\"\"\n");
    {
        CSVParser p("./test_tmp/dquote.csv");
        CHECK(p.parse(), "double-quote CSV: parse ok");
        auto rows = p.getRows();
        CHECK(rows[0][1]=="say \"hello\"", "double-quote: escaped quotes decoded");
    }

    writeTempCSV("./test_tmp/empty_fields.csv","a,b,c\n1,,3\n,2,\n");
    {
        CSVParser p("./test_tmp/empty_fields.csv");
        CHECK(p.parse(), "empty-field CSV: parse ok");
        auto rows = p.getRows();
        CHECK(rows[0][1]=="" && rows[1][0]=="" && rows[1][2]=="",
              "empty fields: correct empty strings");
    }

    writeTempCSV("./test_tmp/crlf.csv","x,y\r\n10,20\r\n30,40\r\n");
    {
        CSVParser p("./test_tmp/crlf.csv");
        CHECK(p.parse(), "CRLF CSV: parse ok");
        auto rows = p.getRows();
        CHECK(rows[0][0]=="10" && rows[1][1]=="40", "CRLF: values correct");
    }

    {
        CSVParser p("./test_tmp/does_not_exist.csv");
        CHECK(!p.parse(), "non-existent file: parse() false");
        CHECK(!p.getLastError().empty(), "non-existent file: error set");
    }
}

// ── 5+6. Writer+Reader NONE ───────────────────────────────────
static void test_writer_reader_int64() {
    SECTION("ColumnWriter + ColumnReader — INT64 (NONE)");
    const std::string dir = "./test_tmp/col_int64";
    removeDir(dir); std::filesystem::create_directories(dir);

    std::vector<int64_t> values = {0,1,-1,42,INT64_MAX,INT64_MIN,99999};
    {
        ColumnWriter w(dir,"num",ColumnType::INT64,Encoding::NONE);
        w.setRowCount(values.size());
        CHECK(w.writeInt64(values), "INT64 write ok");
        CHECK(w.finalize(),         "INT64 finalize ok");
    }
    { std::ifstream f(dir+"/num.col"); CHECK(f.good(), "INT64: .col exists"); }
    { std::ifstream f(dir+"/num.col.tmp"); CHECK(!f.good(), "INT64: .tmp removed"); }
    {
        ColumnReader r;
        CHECK(r.open(dir,"num"),              "INT64 reader: open ok");
        CHECK(r.getType()==ColumnType::INT64, "INT64 reader: type correct");
        CHECK(r.getRowCount()==values.size(), "INT64 reader: row_count correct");
        size_t idx=0;
        while(r.hasNext()) {
            ColumnValue v=r.next();
            CHECK(std::holds_alternative<int64_t>(v), "INT64: variant correct");
            CHECK(std::get<int64_t>(v)==values[idx],
                  "INT64: value["+std::to_string(idx)+"] matches");
            idx++;
        }
        CHECK(idx==values.size(), "INT64: exact row count read");
        r.reset();
        CHECK(r.hasNext(), "INT64: hasNext after reset");
        CHECK(std::get<int64_t>(r.next())==values[0], "INT64: first value after reset");
        r.close();
        CHECK(!r.isOpen(), "INT64: isOpen false after close");
    }
    removeDir(dir);
}

static void test_writer_reader_double() {
    SECTION("ColumnWriter + ColumnReader — DOUBLE (NONE)");
    const std::string dir = "./test_tmp/col_double";
    removeDir(dir); std::filesystem::create_directories(dir);

    std::vector<double> values = {0.0,1.5,-3.14,1e10,-1e-10,99.99};
    {
        ColumnWriter w(dir,"price",ColumnType::DOUBLE,Encoding::NONE);
        w.setRowCount(values.size());
        CHECK(w.writeDouble(values), "DOUBLE write ok");
        CHECK(w.finalize(),           "DOUBLE finalize ok");
    }
    {
        ColumnReader r;
        CHECK(r.open(dir,"price"),             "DOUBLE reader: open ok");
        CHECK(r.getType()==ColumnType::DOUBLE, "DOUBLE reader: type correct");
        size_t idx=0;
        while(r.hasNext()) {
            ColumnValue v=r.next();
            CHECK(std::get<double>(v)==values[idx],
                  "DOUBLE: value["+std::to_string(idx)+"] matches");
            idx++;
        }
        CHECK(idx==values.size(), "DOUBLE: exact row count");
        r.close();
    }
    removeDir(dir);
}

static void test_writer_reader_string() {
    SECTION("ColumnWriter + ColumnReader — STRING (NONE)");
    const std::string dir = "./test_tmp/col_string";
    removeDir(dir); std::filesystem::create_directories(dir);

    std::vector<std::string> values = {"hello","world","","United States","a","with spaces"};
    {
        ColumnWriter w(dir,"country",ColumnType::STRING,Encoding::NONE);
        w.setRowCount(values.size());
        CHECK(w.writeString(values), "STRING write ok");
        CHECK(w.finalize(),           "STRING finalize ok");
    }
    {
        ColumnReader r;
        CHECK(r.open(dir,"country"),           "STRING reader: open ok");
        CHECK(r.getType()==ColumnType::STRING, "STRING reader: type correct");
        size_t idx=0;
        while(r.hasNext()) {
            ColumnValue v=r.next();
            CHECK(std::holds_alternative<std::string>(v), "STRING: variant correct");
            CHECK(std::get<std::string>(v)==values[idx],
                  "STRING: value["+std::to_string(idx)+"] matches");
            idx++;
        }
        CHECK(idx==values.size(), "STRING: exact row count");
        r.close();
    }
    removeDir(dir);
}

static void test_writer_reader_crc_corruption() {
    SECTION("ColumnReader — CRC corruption detection");
    const std::string dir = "./test_tmp/col_crc";
    removeDir(dir); std::filesystem::create_directories(dir);

    {
        ColumnWriter w(dir,"val",ColumnType::INT64,Encoding::NONE);
        w.setRowCount(3); w.writeInt64({10,20,30}); w.finalize();
    }
    // Corrupt byte 20 (lands in data section)
    {
        std::fstream f(dir+"/val.col", std::ios::binary|std::ios::in|std::ios::out);
        f.seekp(20); char bad=0xFF; f.write(&bad,1);
    }
    {
        ColumnReader r;
        CHECK(!r.open(dir,"val"),          "CRC corruption: open() false");
        CHECK(!r.getLastError().empty(),   "CRC corruption: error message set");
    }
    removeDir(dir);
}

static void test_writer_reader_single_row() {
    SECTION("ColumnWriter + ColumnReader — single row edge case");
    const std::string dir = "./test_tmp/col_single";
    removeDir(dir); std::filesystem::create_directories(dir);

    { ColumnWriter w(dir,"x",ColumnType::INT64,Encoding::NONE);
      w.setRowCount(1); w.writeInt64({42}); w.finalize(); }
    { ColumnReader r;
      CHECK(r.open(dir,"x"),     "single row: open ok");
      CHECK(r.getRowCount()==1,  "single row: row_count==1");
      CHECK(r.hasNext(),         "single row: hasNext true");
      CHECK(std::get<int64_t>(r.next())==42, "single row: value==42");
      CHECK(!r.hasNext(),        "single row: hasNext false after read");
      r.close(); }
    removeDir(dir);
}

static void test_writer_reader_empty() {
    SECTION("ColumnWriter + ColumnReader — zero rows edge case");
    const std::string dir = "./test_tmp/col_empty";
    removeDir(dir); std::filesystem::create_directories(dir);

    { ColumnWriter w(dir,"e",ColumnType::INT64,Encoding::NONE);
      w.setRowCount(0); w.writeInt64({}); w.finalize(); }
    { ColumnReader r;
      CHECK(r.open(dir,"e"),    "zero rows: open ok");
      CHECK(r.getRowCount()==0, "zero rows: row_count==0");
      CHECK(!r.hasNext(),       "zero rows: hasNext false immediately");
      r.close(); }
    removeDir(dir);
}

// ── 7. SchemaManager (Phase 1 portion) ───────────────────────
static void test_schema_manager_phase1() {
    SECTION("SchemaManager — Phase 1 (NONE encoding)");
    const std::string dir = "./test_tmp/schema_p1";
    removeDir(dir); std::filesystem::create_directories(dir);

    CHECK(!SchemaManager::schemaExists(dir), "schemaExists: false before write");

    std::vector<SchemaColumn> cols = {
        {"id",      ColumnType::INT64,  Encoding::NONE},
        {"price",   ColumnType::DOUBLE, Encoding::NONE},
        {"country", ColumnType::STRING, Encoding::NONE},
    };
    CHECK(SchemaManager::writeSchema(dir, cols, "test_table"), "writeSchema: ok");
    { std::ifstream tmp(dir+"/schema.json.tmp"); CHECK(!tmp.good(), ".tmp removed"); }
    { std::ifstream fin(dir+"/schema.json");     CHECK(fin.good(),  "schema.json exists"); }
    CHECK(SchemaManager::schemaExists(dir), "schemaExists: true after write");

    auto read = SchemaManager::readSchema(dir);
    CHECK(read.size()==3,                        "readSchema: 3 columns");
    CHECK(read[0].name=="id",                    "readSchema: col0 name");
    CHECK(read[0].type==ColumnType::INT64,       "readSchema: col0 type INT64");
    CHECK(read[0].encoding==Encoding::NONE,      "readSchema: col0 encoding NONE");
    CHECK(read[1].name=="price",                 "readSchema: col1 name");
    CHECK(read[1].type==ColumnType::DOUBLE,      "readSchema: col1 type DOUBLE");
    CHECK(read[2].name=="country",               "readSchema: col2 name");
    CHECK(read[2].type==ColumnType::STRING,      "readSchema: col2 type STRING");

    auto empty = SchemaManager::readSchema("./test_tmp/nonexistent_xyz");
    CHECK(empty.empty(), "readSchema non-existent dir: empty vector");

    removeDir(dir);
}

// ── 8. QueryParser ───────────────────────────────────────────
static void test_query_parser() {
    SECTION("QueryParser (query_parser.cpp)");

    auto parse = [](const std::string& q, QueryPlan& plan) -> bool {
        QueryParser p(q); return p.parse(plan);
    };

    { QueryPlan plan;
      CHECK(parse("SELECT COUNT(*) FROM sales", plan), "parse COUNT(*) FROM");
      CHECK(plan.table=="sales",                 "COUNT(*): table");
      CHECK(plan.selects[0].agg==AggFunc::COUNT, "COUNT(*): agg");
      CHECK(plan.selects[0].star==true,          "COUNT(*): star"); }

    { QueryPlan plan;
      CHECK(parse("SELECT SUM(price) FROM sales WHERE date >= 20240101", plan),
            "parse SUM WHERE");
      CHECK(plan.selects[0].agg==AggFunc::SUM,  "SUM WHERE: agg");
      CHECK(plan.selects[0].col=="price",        "SUM WHERE: col");
      CHECK(plan.where.has_where,                "SUM WHERE: has_where");
      CHECK(plan.where.col=="date",              "SUM WHERE: col");
      CHECK(plan.where.op==">=",                 "SUM WHERE: op");
      CHECK(plan.where.val=="20240101",          "SUM WHERE: val"); }

    { QueryPlan plan;
      CHECK(parse("SELECT * FROM inventory", plan), "parse SELECT *");
      CHECK(plan.select_star, "SELECT *: select_star"); }

    { QueryPlan plan;
      CHECK(parse("SELECT id, name FROM users", plan), "parse multi-col SELECT");
      CHECK(plan.selects.size()==2 && plan.selects[0].col=="id", "multi-col: cols"); }

    { QueryPlan plan;
      CHECK(parse("SELECT country, SUM(price) FROM sales GROUP BY country", plan),
            "parse GROUP BY");
      CHECK(plan.groupby=="country", "GROUP BY: groupby col"); }

    for (const auto& op : std::vector<std::string>{"=","!=","<","<=",">",">="}) {
        QueryPlan plan;
        std::string q = "SELECT x FROM t WHERE y " + op + " 5";
        CHECK(parse(q,plan) && plan.where.op==op, "WHERE op: "+op);
    }

    { QueryPlan plan;
      CHECK(parse("SELECT x FROM t WHERE cat = 'Electronics'", plan),
            "parse string literal WHERE");
      CHECK(plan.where.val=="Electronics", "string literal: no quotes in val"); }

    { QueryPlan plan;
      CHECK(parse("SELECT AVG(qty), MIN(price), MAX(price) FROM t", plan),
            "parse AVG MIN MAX");
      CHECK(plan.selects[0].agg==AggFunc::AVG, "AVG parsed");
      CHECK(plan.selects[1].agg==AggFunc::MIN, "MIN parsed");
      CHECK(plan.selects[2].agg==AggFunc::MAX, "MAX parsed"); }

    { QueryPlan plan;
      CHECK(parse("select sum(price) from sales where date >= 1", plan),
            "lowercase keywords parsed");
      CHECK(plan.selects[0].agg==AggFunc::SUM, "lowercase: SUM"); }

    // neededColumns
    { QueryPlan plan;
      parse("SELECT country, SUM(price) FROM sales WHERE date >= 1", plan);
      auto nc = plan.neededColumns();
      CHECK(nc.size()==3, "neededColumns: 3 unique");
      bool hc=false,hp=false,hd=false;
      for(auto& c:nc){ if(c=="country")hc=true; if(c=="price")hp=true; if(c=="date")hd=true; }
      CHECK(hc&&hp&&hd, "neededColumns: all 3 present"); }

    { QueryPlan plan;
      parse("SELECT price FROM t WHERE price > 5", plan);
      CHECK(plan.neededColumns().size()==1, "neededColumns: no duplicates"); }

    // Syntax errors
    { QueryPlan plan;
      QueryParser p1("FROM t"); CHECK(!p1.parse(plan), "error: missing SELECT");
      QueryParser p2("SELECT x"); CHECK(!p2.parse(plan), "error: missing FROM");
      QueryParser p3(""); CHECK(!p3.parse(plan), "error: empty query"); }
}

// ── 9. PredicateEvaluator ────────────────────────────────────
static void test_predicate_evaluator() {
    SECTION("PredicateEvaluator (predicate.cpp)");
    const std::string dir = "./test_tmp/pred";
    removeDir(dir); std::filesystem::create_directories(dir);

    { ColumnWriter w(dir,"val",ColumnType::INT64,Encoding::NONE);
      w.setRowCount(5); w.writeInt64({10,20,30,40,50}); w.finalize(); }

    auto eval = [&](const std::string& op, const std::string& lit) -> Bitmap {
        WherePredicate p; p.has_where=true; p.col="val"; p.op=op; p.val=lit;
        return PredicateEvaluator::evaluate(p, dir, 5);
    };

    { auto bm=eval("=","30");
      CHECK(bm.size()==5 && !bm[0]&&!bm[1]&&bm[2]&&!bm[3]&&!bm[4],
            "predicate =: only row 2");
      CHECK(PredicateEvaluator::countTrue(bm)==1, "predicate =: countTrue 1"); }

    { auto bm=eval("!=","30");
      CHECK(PredicateEvaluator::countTrue(bm)==4, "predicate !=: 4 pass");
      CHECK(!bm[2], "predicate !=: row 2 excluded"); }

    { auto bm=eval("<","30");
      CHECK(PredicateEvaluator::countTrue(bm)==2 && bm[0]&&bm[1]&&!bm[2],
            "predicate <: rows 0,1"); }

    { auto bm=eval("<=","30");
      CHECK(PredicateEvaluator::countTrue(bm)==3, "predicate <=: 3 pass"); }

    { auto bm=eval(">","30");
      CHECK(PredicateEvaluator::countTrue(bm)==2 && bm[3]&&bm[4],
            "predicate >: rows 3,4"); }

    { auto bm=eval(">=","30");
      CHECK(PredicateEvaluator::countTrue(bm)==3, "predicate >=: 3 pass"); }

    { WherePredicate nw; nw.has_where=false;
      auto bm=PredicateEvaluator::evaluate(nw, dir, 5);
      CHECK(PredicateEvaluator::countTrue(bm)==5, "no WHERE: all 5 pass"); }

    { auto bm=eval("=","999");
      CHECK(PredicateEvaluator::countTrue(bm)==0, "predicate = 999: 0 pass"); }

    // String predicate
    { ColumnWriter w(dir,"cat",ColumnType::STRING,Encoding::NONE);
      w.setRowCount(4);
      w.writeString({"Electronics","Books","Electronics","Clothing"});
      w.finalize();
      WherePredicate p; p.has_where=true; p.col="cat"; p.op="="; p.val="Electronics";
      auto bm=PredicateEvaluator::evaluate(p, dir, 4);
      CHECK(PredicateEvaluator::countTrue(bm)==2, "string pred: 2 matches");
      CHECK(bm[0]&&!bm[1]&&bm[2]&&!bm[3], "string pred: rows 0,2"); }

    // evaluateWithReader resets reader
    { ColumnReader r; r.open(dir,"val");
      WherePredicate p; p.has_where=true; p.col="val"; p.op=">"; p.val="25";
      auto bm=PredicateEvaluator::evaluateWithReader(p, r);
      CHECK(bm.size()==5, "evaluateWithReader: bitmap size");
      CHECK(r.hasNext(), "evaluateWithReader: reader reset after call");
      r.close(); }

    { Bitmap all_true(10,true), all_false(10,false);
      CHECK(PredicateEvaluator::countTrue(all_true)==10,  "countTrue all-true: 10");
      CHECK(PredicateEvaluator::countTrue(all_false)==0,  "countTrue all-false: 0");
      CHECK(PredicateEvaluator::countTrue({})==0,         "countTrue empty: 0"); }

    removeDir(dir);
}

// ── 10. End-to-end Phase 1 ────────────────────────────────────
static void test_end_to_end_phase1() {
    SECTION("End-to-end Phase 1: multi-column write + read + predicate");
    const std::string dir = "./test_tmp/e2e_p1";
    removeDir(dir); std::filesystem::create_directories(dir);

    std::vector<int64_t>     ids    = {1,2,3,4,5};
    std::vector<double>      prices = {9.99,24.99,4.99,49.99,14.99};
    std::vector<std::string> cats   = {"Books","Electronics","Books","Electronics","Clothing"};

    { ColumnWriter wi(dir,"id",   ColumnType::INT64, Encoding::NONE);
      wi.setRowCount(5); wi.writeInt64(ids); wi.finalize();
      ColumnWriter wp(dir,"price",ColumnType::DOUBLE,Encoding::NONE);
      wp.setRowCount(5); wp.writeDouble(prices); wp.finalize();
      ColumnWriter wc(dir,"cat",  ColumnType::STRING,Encoding::NONE);
      wc.setRowCount(5); wc.writeString(cats); wc.finalize();
      SchemaManager::writeSchema(dir, {
          {"id",ColumnType::INT64,Encoding::NONE},
          {"price",ColumnType::DOUBLE,Encoding::NONE},
          {"cat",ColumnType::STRING,Encoding::NONE}}, "sales"); }

    auto schema = SchemaManager::readSchema(dir);
    CHECK(schema.size()==3, "e2e p1: schema 3 cols");

    ColumnReader r_price, r_cat;
    CHECK(r_price.open(dir,"price"), "e2e p1: price reader open");
    CHECK(r_cat.open(dir,"cat"),     "e2e p1: cat reader open");

    WherePredicate pred; pred.has_where=true; pred.col="price"; pred.op=">"; pred.val="10.0";
    auto bm = PredicateEvaluator::evaluateWithReader(pred, r_price);
    CHECK(!bm[0]&&bm[1]&&!bm[2]&&bm[3]&&bm[4], "e2e p1: predicate bitmap correct");
    CHECK(PredicateEvaluator::countTrue(bm)==3,  "e2e p1: 3 rows pass");

    double sum=0.0; size_t row=0;
    while(r_price.hasNext()) {
        ColumnValue v=r_price.next();
        if(bm[row]) sum+=std::get<double>(v);
        row++;
    }
    CHECK(std::abs(sum-89.97)<0.001, "e2e p1: SUM(price) WHERE price>10 = 89.97");

    r_price.close(); r_cat.close();
    removeDir(dir);
}

// ============================================================
// ========== PHASE 2 TESTS ===================================
// ============================================================

// ── 11a. ColumnReader DICTIONARY — via manual helper ─────────
static void test_reader_dictionary() {
    SECTION("Phase 2: ColumnReader — DICTIONARY decoding");
    const std::string dir = "./test_tmp/dict_reader";
    removeDir(dir); std::filesystem::create_directories(dir);

    // Write a DICTIONARY column using the manual helper
    std::vector<std::string> values = {
        "USA","UK","Germany","USA","Germany","USA","UK","USA"
    };
    CHECK(writeDictColumn(dir, "country", values),
          "DICT helper: column written successfully");

    // Verify .col exists and .tmp is gone
    { std::ifstream f(dir+"/country.col"); CHECK(f.good(), "DICT: .col file exists"); }
    { std::ifstream f(dir+"/country.col.tmp"); CHECK(!f.good(), "DICT: .tmp removed"); }

    ColumnReader r;
    CHECK(r.open(dir,"country"),            "DICT reader: open ok");
    CHECK(r.isOpen(),                       "DICT reader: isOpen true");
    CHECK(r.getEncoding()==Encoding::DICTIONARY, "DICT reader: encoding is DICTIONARY");
    CHECK(r.getType()==ColumnType::STRING,  "DICT reader: type is STRING");
    CHECK(r.getRowCount()==values.size(),   "DICT reader: row_count correct");

    // Read all values back and compare
    size_t idx=0;
    while(r.hasNext()) {
        ColumnValue v = r.next();
        CHECK(std::holds_alternative<std::string>(v),
              "DICT: value["+std::to_string(idx)+"] is string variant");
        CHECK(std::get<std::string>(v)==values[idx],
              "DICT: value["+std::to_string(idx)+"] == "+values[idx]);
        idx++;
    }
    CHECK(idx==values.size(), "DICT: exact row count read");

    // reset() and re-read first value
    r.reset();
    CHECK(r.hasNext(), "DICT: hasNext true after reset");
    ColumnValue first = r.next();
    CHECK(std::get<std::string>(first)==values[0], "DICT: first value correct after reset");

    r.close();
    CHECK(!r.isOpen(), "DICT: isOpen false after close");

    removeDir(dir);
}

// ── 11b. DICTIONARY — single distinct value ──────────────────
static void test_reader_dictionary_single_distinct() {
    SECTION("Phase 2: ColumnReader DICTIONARY — single distinct value");
    const std::string dir = "./test_tmp/dict_single";
    removeDir(dir); std::filesystem::create_directories(dir);

    // All rows have the same value — still valid dictionary encoding
    std::vector<std::string> values(100, "Electronics");
    CHECK(writeDictColumn(dir,"cat",values), "DICT single: write ok");

    ColumnReader r;
    CHECK(r.open(dir,"cat"), "DICT single: open ok");
    CHECK(r.getRowCount()==100, "DICT single: row_count 100");

    size_t idx=0;
    while(r.hasNext()) {
        ColumnValue v=r.next();
        CHECK(std::get<std::string>(v)=="Electronics",
              "DICT single: row "+std::to_string(idx)+" is Electronics");
        idx++;
    }
    CHECK(idx==100, "DICT single: all 100 rows read");
    r.close();
    removeDir(dir);
}

// ── 11c. DICTIONARY — many distinct values ───────────────────
static void test_reader_dictionary_many_distinct() {
    SECTION("Phase 2: ColumnReader DICTIONARY — 87 distinct values");
    const std::string dir = "./test_tmp/dict_many";
    removeDir(dir); std::filesystem::create_directories(dir);

    // Build 87 distinct category strings, repeated a few times
    std::vector<std::string> cats;
    for(int i=0;i<87;i++) cats.push_back("cat_"+std::to_string(i));
    std::vector<std::string> values;
    for(int rep=0;rep<5;rep++)
        for(const auto& c:cats) values.push_back(c);

    CHECK(writeDictColumn(dir,"category",values), "DICT many: write ok");

    ColumnReader r;
    CHECK(r.open(dir,"category"),    "DICT many: open ok");
    CHECK(r.getRowCount()==values.size(), "DICT many: row_count correct");

    size_t idx=0;
    while(r.hasNext()) {
        ColumnValue v=r.next();
        CHECK(std::get<std::string>(v)==values[idx],
              "DICT many: value["+std::to_string(idx)+"] matches");
        idx++;
    }
    CHECK(idx==values.size(), "DICT many: all rows read");
    r.close();
    removeDir(dir);
}

// ── 12a. ColumnReader RLE — basic ────────────────────────────
static void test_reader_rle() {
    SECTION("Phase 2: ColumnReader — RLE decoding");
    const std::string dir = "./test_tmp/rle_reader";
    removeDir(dir); std::filesystem::create_directories(dir);

    // Sorted date-like column: many repeated values
    std::vector<int64_t> values;
    for(int d=1;d<=5;d++)
        for(int r=0;r<10;r++) values.push_back(d);  // 5 distinct, 10 each = 50 rows

    CHECK(writeRLEColumn(dir,"date",values), "RLE: write ok");
    { std::ifstream f(dir+"/date.col"); CHECK(f.good(), "RLE: .col exists"); }
    { std::ifstream f(dir+"/date.col.tmp"); CHECK(!f.good(), "RLE: .tmp removed"); }

    ColumnReader r;
    CHECK(r.open(dir,"date"),             "RLE reader: open ok");
    CHECK(r.getEncoding()==Encoding::RLE, "RLE reader: encoding is RLE");
    CHECK(r.getType()==ColumnType::INT64, "RLE reader: type is INT64");
    CHECK(r.getRowCount()==values.size(), "RLE reader: row_count correct");

    size_t idx=0;
    while(r.hasNext()) {
        ColumnValue v=r.next();
        CHECK(std::holds_alternative<int64_t>(v),
              "RLE: value["+std::to_string(idx)+"] is int64");
        CHECK(std::get<int64_t>(v)==values[idx],
              "RLE: value["+std::to_string(idx)+"] == "+std::to_string(values[idx]));
        idx++;
    }
    CHECK(idx==values.size(), "RLE: exact row count read");

    r.reset();
    CHECK(r.hasNext(), "RLE: hasNext after reset");
    CHECK(std::get<int64_t>(r.next())==values[0], "RLE: first value after reset");

    r.close();
    CHECK(!r.isOpen(), "RLE: isOpen false after close");

    removeDir(dir);
}

// ── 12b. RLE — all same value ─────────────────────────────────
static void test_reader_rle_all_same() {
    SECTION("Phase 2: ColumnReader RLE — all same value (1 run)");
    const std::string dir = "./test_tmp/rle_same";
    removeDir(dir); std::filesystem::create_directories(dir);

    std::vector<int64_t> values(1000, 20240101LL);
    CHECK(writeRLEColumn(dir,"date",values), "RLE all-same: write ok");

    ColumnReader r;
    CHECK(r.open(dir,"date"),    "RLE all-same: open ok");
    CHECK(r.getRowCount()==1000, "RLE all-same: row_count 1000");

    // Read all values without per-row CHECK to avoid spam
    size_t count=0;
    bool all_correct = true;
    while(r.hasNext()) {
        ColumnValue v=r.next();
        if(std::get<int64_t>(v)!=20240101LL) all_correct=false;
        count++;
    }
    CHECK(count==1000, "RLE all-same: all 1000 rows read");
    CHECK(all_correct, "RLE all-same: all values == 20240101");

    r.close();
    removeDir(dir);
}

// ── 12c. RLE — alternating values (many short runs) ──────────
static void test_reader_rle_alternating() {
    SECTION("Phase 2: ColumnReader RLE — alternating values");
    const std::string dir = "./test_tmp/rle_alt";
    removeDir(dir); std::filesystem::create_directories(dir);

    // Alternating: run length 1 each time (worst case for RLE space)
    std::vector<int64_t> values;
    for(int i=0;i<20;i++) values.push_back(i%2==0 ? 1 : 2);

    CHECK(writeRLEColumn(dir,"flag",values), "RLE alternating: write ok");

    ColumnReader r;
    CHECK(r.open(dir,"flag"),    "RLE alternating: open ok");
    CHECK(r.getRowCount()==values.size(), "RLE alternating: row_count");

    size_t idx=0;
    bool all_correct = true;
    while(r.hasNext()) {
        ColumnValue v=r.next();
        if(std::get<int64_t>(v)!=values[idx]) all_correct=false;
        idx++;
    }
    CHECK(idx==values.size(), "RLE alternating: all rows read");
    CHECK(all_correct, "RLE alternating: all values correct");

    r.close();
    removeDir(dir);
}

// ── 13. SchemaManager Phase 2: DICTIONARY and RLE round-trip ─
static void test_schema_manager_phase2() {
    SECTION("Phase 2: SchemaManager — DICTIONARY and RLE encoding round-trip");
    const std::string dir = "./test_tmp/schema_p2";
    removeDir(dir); std::filesystem::create_directories(dir);

    // Write schema with all three encoding types
    std::vector<SchemaColumn> cols = {
        {"id",       ColumnType::INT64,  Encoding::NONE},
        {"date",     ColumnType::INT64,  Encoding::RLE},
        {"country",  ColumnType::STRING, Encoding::DICTIONARY},
        {"category", ColumnType::STRING, Encoding::DICTIONARY},
        {"price",    ColumnType::DOUBLE, Encoding::NONE},
    };
    CHECK(SchemaManager::writeSchema(dir, cols, "sales"), "schema p2: writeSchema ok");

    auto read = SchemaManager::readSchema(dir);
    CHECK(read.size()==5, "schema p2: 5 columns");

    CHECK(read[0].name=="id"       && read[0].encoding==Encoding::NONE,
          "schema p2: id -> NONE");
    CHECK(read[1].name=="date"     && read[1].encoding==Encoding::RLE,
          "schema p2: date -> RLE");
    CHECK(read[2].name=="country"  && read[2].encoding==Encoding::DICTIONARY,
          "schema p2: country -> DICTIONARY");
    CHECK(read[3].name=="category" && read[3].encoding==Encoding::DICTIONARY,
          "schema p2: category -> DICTIONARY");
    CHECK(read[4].name=="price"    && read[4].encoding==Encoding::NONE,
          "schema p2: price -> NONE");

    // Types must also survive the round-trip
    CHECK(read[1].type==ColumnType::INT64,  "schema p2: date type INT64");
    CHECK(read[2].type==ColumnType::STRING, "schema p2: country type STRING");

    removeDir(dir);
}

// ── 14. Encoding selection logic ─────────────────────────────
static void test_encoding_selection() {
    SECTION("Phase 2: Loader encoding-selection logic");
    const std::string warehouse = "./test_tmp/enc_sel_wh";
    removeDir(warehouse);

    std::ostringstream csv;
    csv << "id,date,country,price\n";
    const char* countries[] = {"USA","UK","Germany","France","Japan",
                               "Canada","Australia","Brazil","India","Mexico",
                               "Italy","Spain","Korea","Sweden","Norway"};
    for(int i=1; i<=100; i++) {
        int date = (i<=33) ? 20240101 : (i<=66) ? 20240102 : 20240103;
        const char* country = countries[i%15];
        double price = 10.0 + (i*3.7);
        csv << i << "," << date << "," << country << ","
            << std::fixed << std::setprecision(2) << price << "\n";
    }
    writeTempCSV("./test_tmp/enc_sel.csv", csv.str());

    bool loaded = Loader::load("./test_tmp/enc_sel.csv", "enc_test", warehouse);
    CHECK(loaded, "encoding selection: Loader::load returns true");

    auto schema = SchemaManager::readSchema(warehouse+"/enc_test");
    CHECK(schema.size()==4, "encoding selection: 4 columns in schema");

    // id: high-cardinality int -> NONE
    CHECK(schema[0].name=="id" && schema[0].encoding==Encoding::NONE,
          "encoding selection: id -> NONE");

    // date: sorted int, 3 distinct, 100 rows -> 3 runs << 25 -> RLE
    CHECK(schema[1].name=="date" && schema[1].encoding==Encoding::RLE,
          "encoding selection: date -> RLE (3 runs << N/4)");

    // country: STRING -> DICTIONARY always
    CHECK(schema[2].name=="country" && schema[2].encoding==Encoding::DICTIONARY,
          "encoding selection: country -> DICTIONARY");

    // price: random doubles -> NONE
    CHECK(schema[3].name=="price" && schema[3].encoding==Encoding::NONE,
          "encoding selection: price -> NONE");

    ColumnReader cr;
    CHECK(cr.open(warehouse+"/enc_test","country"),
          "encoding selection: country reader opens");
    CHECK(cr.getEncoding()==Encoding::DICTIONARY,
          "encoding selection: country reader sees DICTIONARY encoding");
    CHECK(cr.getRowCount()==100, "encoding selection: country has 100 rows");

    size_t count=0;
    while(cr.hasNext()) { cr.next(); count++; }
    CHECK(count==100, "encoding selection: country reader decoded all 100 rows");
    cr.close();

    ColumnReader dr;
    CHECK(dr.open(warehouse+"/enc_test","date"),
          "encoding selection: date reader opens");
    CHECK(dr.getEncoding()==Encoding::RLE,
          "encoding selection: date reader sees RLE encoding");
    count=0;
    while(dr.hasNext()) { dr.next(); count++; }
    CHECK(count==100, "encoding selection: date reader decoded all 100 rows");
    dr.close();

    removeDir(warehouse);
    std::remove("./test_tmp/enc_sel.csv");
}

// ── 15. GROUP BY end-to-end ───────────────────────────────────
static void test_group_by_executor() {
    SECTION("Phase 2: Executor GROUP BY with DICTIONARY column");
    const std::string dir = "./test_tmp/groupby";
    removeDir(dir); std::filesystem::create_directories(dir);

    std::vector<std::string> countries = {"USA","UK","USA","UK","Germany"};
    std::vector<double>      prices    = {10.0,20.0,30.0,40.0,50.0};

    CHECK(writeDictColumn(dir,"country",countries), "GROUP BY: country dict col written");

    { ColumnWriter w(dir,"price",ColumnType::DOUBLE,Encoding::NONE);
      w.setRowCount(5); w.writeDouble(prices); w.finalize(); }

    std::vector<SchemaColumn> schema_cols = {
        {"country", ColumnType::STRING, Encoding::DICTIONARY},
        {"price",   ColumnType::DOUBLE, Encoding::NONE},
    };
    CHECK(SchemaManager::writeSchema(dir, schema_cols, "sales"), "GROUP BY: schema written");

    ColumnReader r_country, r_price;
    CHECK(r_country.open(dir,"country"), "GROUP BY: country reader opens");
    CHECK(r_price.open(dir,"price"),     "GROUP BY: price reader opens");

    QueryPlan plan;
    plan.table   = "sales";
    plan.groupby = "country";
    plan.select_star = false;
    { SelectExpr e1; e1.agg=AggFunc::NONE; e1.col="country"; plan.selects.push_back(e1); }
    { SelectExpr e2; e2.agg=AggFunc::SUM;  e2.col="price";   plan.selects.push_back(e2); }

    std::vector<ColumnReader*> readers = {&r_country, &r_price};
    std::vector<std::string>   colNames = {"country","price"};
    Bitmap bitmap(5, true);

    std::ostringstream captured;
    std::streambuf* old_buf = std::cout.rdbuf(captured.rdbuf());

    bool ok = Executor::run(plan, bitmap, readers, colNames, schema_cols, dir);

    std::cout.rdbuf(old_buf);
    std::string out = captured.str();

    CHECK(ok, "GROUP BY executor: run() returned true");

    bool has_usa     = out.find("USA")     != std::string::npos;
    bool has_uk      = out.find("UK")      != std::string::npos;
    bool has_germany = out.find("Germany") != std::string::npos;
    CHECK(has_usa && has_uk && has_germany, "GROUP BY: all 3 group keys appear in output");

    bool has_40 = out.find("40.00") != std::string::npos ||
                  out.find("40")    != std::string::npos;
    bool has_60 = out.find("60.00") != std::string::npos ||
                  out.find("60")    != std::string::npos;
    bool has_50 = out.find("50.00") != std::string::npos ||
                  out.find("50")    != std::string::npos;
    CHECK(has_40, "GROUP BY: USA SUM(price)=40 in output");
    CHECK(has_60, "GROUP BY: UK SUM(price)=60 in output");
    CHECK(has_50, "GROUP BY: Germany SUM(price)=50 in output");

    CHECK(out.find("groups") != std::string::npos ||
          out.find("ms")     != std::string::npos,
          "GROUP BY: timing/stats line present");

    r_country.close(); r_price.close();
    removeDir(dir);
}

// ── 16. Phase 2 compression ratio sanity check ───────────────
static void test_compression_ratio() {
    SECTION("Phase 2: Compression ratio sanity check");
    const std::string dir = "./test_tmp/compression";
    removeDir(dir); std::filesystem::create_directories(dir);

    std::vector<std::string> countries;
    const char* vals[] = {"United States","United Kingdom","Germany","France","Japan"};
    for(int i=0;i<1000;i++) countries.push_back(vals[i%5]);

    CHECK(writeDictColumn(dir,"country_dict", countries), "compression: dict write ok");

    { ColumnWriter w(dir,"country_none",ColumnType::STRING,Encoding::NONE);
      w.setRowCount(1000); w.writeString(countries); w.finalize(); }

    size_t dict_size=0, none_size=0;
    { std::ifstream f(dir+"/country_dict.col", std::ios::ate); dict_size=f.tellg(); }
    { std::ifstream f(dir+"/country_none.col", std::ios::ate); none_size=f.tellg(); }

    CHECK(dict_size < none_size,
          "compression: DICTIONARY col smaller than NONE ("+
          std::to_string(dict_size)+" < "+std::to_string(none_size)+")");

    double ratio = static_cast<double>(none_size) / dict_size;
    CHECK(ratio > 2.0,
          "compression: DICTIONARY ratio > 2x (actual: "+
          std::to_string(ratio).substr(0,4)+"x)");

    std::vector<int64_t> dates;
    for(int d=1;d<=10;d++)
        for(int r=0;r<100;r++) dates.push_back(20240100+d);

    CHECK(writeRLEColumn(dir,"date_rle",dates), "compression: rle write ok");
    { ColumnWriter w(dir,"date_none",ColumnType::INT64,Encoding::NONE);
      w.setRowCount(1000); w.writeInt64(dates); w.finalize(); }

    size_t rle_size=0, none_date_size=0;
    { std::ifstream f(dir+"/date_rle.col",  std::ios::ate); rle_size=f.tellg(); }
    { std::ifstream f(dir+"/date_none.col", std::ios::ate); none_date_size=f.tellg(); }

    CHECK(rle_size < none_date_size,
          "compression: RLE col smaller than NONE ("+
          std::to_string(rle_size)+" < "+std::to_string(none_date_size)+")");

    removeDir(dir);
}

// ── 17. DICTIONARY predicate (WHERE on dict column) ──────────
static void test_predicate_on_dict_column() {
    SECTION("Phase 2: PredicateEvaluator on DICTIONARY column");
    const std::string dir = "./test_tmp/pred_dict";
    removeDir(dir); std::filesystem::create_directories(dir);

    std::vector<std::string> values = {
        "Electronics","Books","Electronics","Clothing","Books","Electronics"
    };
    CHECK(writeDictColumn(dir,"category",values), "pred dict: write ok");

    WherePredicate pred;
    pred.has_where=true; pred.col="category"; pred.op="="; pred.val="Electronics";
    auto bm = PredicateEvaluator::evaluate(pred, dir, values.size());

    CHECK(bm.size()==6, "pred dict: bitmap size 6");
    CHECK(bm[0]&&!bm[1]&&bm[2]&&!bm[3]&&!bm[4]&&bm[5],
          "pred dict: rows 0,2,5 pass (Electronics)");
    CHECK(PredicateEvaluator::countTrue(bm)==3, "pred dict: countTrue==3");

    WherePredicate pred2;
    pred2.has_where=true; pred2.col="category"; pred2.op="!="; pred2.val="Electronics";
    auto bm2 = PredicateEvaluator::evaluate(pred2, dir, values.size());
    CHECK(PredicateEvaluator::countTrue(bm2)==3, "pred dict: != Electronics -> 3 pass");

    removeDir(dir);
}

// ── 18. RLE predicate (WHERE on RLE column) ──────────────────
static void test_predicate_on_rle_column() {
    SECTION("Phase 2: PredicateEvaluator on RLE column");
    const std::string dir = "./test_tmp/pred_rle";
    removeDir(dir); std::filesystem::create_directories(dir);

    std::vector<int64_t> values;
    for(int d=1;d<=3;d++)
        for(int r=0;r<5;r++) values.push_back(20240100+d);

    CHECK(writeRLEColumn(dir,"date",values), "pred rle: write ok");

    WherePredicate pred;
    pred.has_where=true; pred.col="date"; pred.op=">="; pred.val="20240102";
    auto bm = PredicateEvaluator::evaluate(pred, dir, values.size());

    CHECK(bm.size()==15, "pred rle: bitmap size 15");
    CHECK(PredicateEvaluator::countTrue(bm)==10, "pred rle: >= 20240102 -> 10 rows");

    bool first_five_false = !bm[0]&&!bm[1]&&!bm[2]&&!bm[3]&&!bm[4];
    CHECK(first_five_false, "pred rle: first 5 rows excluded");

    bool last_ten_true = true;
    for(int i=5;i<15;i++) if(!bm[i]) last_ten_true=false;
    CHECK(last_ten_true, "pred rle: last 10 rows pass");

    removeDir(dir);
}

// ============================================================
// main
// ============================================================
int main() {
    ensureBaseDir();

    std::cout << "============================================\n";
    std::cout << "  Phase 1 + Phase 2 Test Suite\n";
    std::cout << "============================================\n";

    // ── Phase 1 ──────────────────────────────────────────
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
    test_schema_manager_phase1();
    test_query_parser();
    test_predicate_evaluator();
    test_end_to_end_phase1();

    // ── Phase 2 ──────────────────────────────────────────
    test_reader_dictionary();
    test_reader_dictionary_single_distinct();
    test_reader_dictionary_many_distinct();
    test_reader_rle();
    test_reader_rle_all_same();
    test_reader_rle_alternating();
    test_schema_manager_phase2();
    test_encoding_selection();
    test_group_by_executor();
    test_compression_ratio();
    test_predicate_on_dict_column();
    test_predicate_on_rle_column();

    // ── Summary ──────────────────────────────────────────
    std::cout << "\n============================================\n";
    std::cout << "  Results: " << g_passed << " passed, "
              << g_failed << " failed\n";
    std::cout << "============================================\n";

    return g_failed == 0 ? 0 : 1;
}