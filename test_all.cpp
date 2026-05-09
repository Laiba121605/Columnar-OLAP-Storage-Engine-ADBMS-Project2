// ============================================================
// test_all.cpp  --  Full Phase 1 + Phase 2 Test Suite
// 300+ tests covering every requirement in the project manual
//
// SECTIONS:
//   1.  CRC64                          (common.cpp)
//   2.  Type / Encoding converters     (common.cpp)
//   3.  ColumnHeader struct layout     (common.h)
//   4.  TypeInference                  (type_inference.cpp)
//   5.  CSVParser                      (csv_parser.cpp)
//   6.  ColumnWriter + Reader NONE     (Phase 1 raw encoding)
//   7.  ColumnWriter + Reader DICT     (Phase 2 dictionary)
//   8.  ColumnWriter + Reader RLE      (Phase 2 RLE)
//   9.  Encoding selection logic       (Phase 2 -- via Loader)
//   10. SchemaManager                  (schema.cpp)
//   11. QueryParser                    (query_parser.cpp)
//   12. PredicateEvaluator             (predicate.cpp)
//   13. End-to-end Phase 1 queries     (NONE encoding)
//   14. End-to-end Phase 2 queries     (DICT + RLE encoding)
//   15. GROUP BY with DICT columns     (Phase 2 executor)
//   16. File format spec compliance    (Section 4)
//   17. Loader integration             (Section 5)
//
// HOW TO COMPILE (WSL / Ubuntu):
//   g++ -std=c++17 -Wall -o test_all test_all.cpp \
//       src/common.cpp src/type_inference.cpp src/csv_parser.cpp \
//       src/column_writer.cpp src/column_reader.cpp src/schema.cpp \
//       src/query_parser.cpp src/predicate.cpp src/executor.cpp \
//       src/loader.cpp -I include && ./test_all
//
// HOW TO COMPILE (Windows Developer Command Prompt):
//   cl /std:c++17 /I include /EHsc test_all.cpp src\common.cpp \
//      src\type_inference.cpp src\csv_parser.cpp src\column_writer.cpp \
//      src\column_reader.cpp src\schema.cpp src\query_parser.cpp \
//      src\predicate.cpp src\executor.cpp src\loader.cpp \
//      /Fe:test_all.exe && test_all.exe
// ============================================================

#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <variant>
#include <cstdio>
#include <cstdlib>  // getenv for temp directory
#include <cstring>
#include <cmath>
#include <filesystem>
#include <deque>

#include "include/common.h"
#include "include/type_inference.h"
#include "include/csv_parser.h"
#include "include/column_writer.h"
#include "include/column_reader.h"
#include "include/schema.h"
#include "include/query_parser.h"
#include "include/predicate.h"
#include "include/executor.h"
#include "include/loader.h"

// ============================================================
// Tiny test harness
// ============================================================
static int g_passed = 0;
static int g_failed = 0;
static int g_section_passed = 0;
static int g_section_failed = 0;

static void CHECK(bool condition, const std::string& desc) {
    if (condition) {
        std::cout << "  [PASS] " << desc << "\n";
        g_passed++;
        g_section_passed++;
    } else {
        std::cout << "  [FAIL] " << desc << "\n";
        g_failed++;
        g_section_failed++;
    }
    std::cout.flush();  // flush after every check to survive crashes
}

static void SECTION(const std::string& name) {
    if (g_section_passed + g_section_failed > 0) {
        std::cout << "         (" << g_section_passed << " passed";
        if (g_section_failed) std::cout << ", " << g_section_failed << " FAILED";
        std::cout << ")\n";
    }
    g_section_passed = 0;
    g_section_failed = 0;
    std::cout << "\n-- " << name << " --\n";
}

// ============================================================
// Helpers
// ============================================================
static void writeFile(const std::string& path, const std::string& content) {
    std::ofstream f(path, std::ios::binary);
    if (f.is_open()) {
        f.write(content.c_str(), content.size());
        f.flush();
        f.close();
    }
}

static void mkdirP(const std::string& path) {
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
}

static void rmDir(const std::string& path) {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
}

// Get size of a file in bytes, -1 if not found
static long fileSize(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.is_open()) return -1;
    return static_cast<long>(f.tellg());
}

// Read all values from an open reader into a vector
static std::vector<ColumnValue> readAll(ColumnReader& r) {
    std::vector<ColumnValue> out;
    while (r.hasNext()) out.push_back(r.next());
    return out;
}

// Helper to write+finalize a simple INT64 column and return file size
static long writeInt64Col(const std::string& dir, const std::string& name,
                          const std::vector<int64_t>& vals, Encoding enc = Encoding::NONE) {
    mkdirP(dir);
    ColumnWriter w(dir, name, ColumnType::INT64, enc);
    w.setRowCount(vals.size());
    if (enc == Encoding::RLE) w.writeRLE_Int64(vals);
    else                      w.writeInt64(vals);
    w.finalize();
    return fileSize(dir + "/" + name + ".col");
}

static long writeStringCol(const std::string& dir, const std::string& name,
                            const std::vector<std::string>& vals, Encoding enc = Encoding::NONE) {
    mkdirP(dir);
    ColumnWriter w(dir, name, ColumnType::STRING, enc);
    w.setRowCount(vals.size());
    if (enc == Encoding::DICTIONARY) w.writeDictionary(vals);
    else                             w.writeString(vals);
    w.finalize();
    return fileSize(dir + "/" + name + ".col");
}

// Base temp directory -- avoids /tmp on Windows
// Use a fixed subdirectory of the current working directory.
// We know the CWD is writable because cl.exe writes .obj files there.
// Using TEMP failed due to Windows Defender blocking untrusted process I/O.
static const std::string BASE = "colsh_test_tmp";

// ============================================================
// SECTION 1 -- CRC64
// ============================================================
static void test_crc64() {
    SECTION("1. CRC64 (common.cpp -- Section 4 footer_crc64)");

    // Manual Section 4: footer_crc64 covers everything above it
    CHECK(crc64(nullptr, 0) == 0,
          "CRC64 of empty input is 0");

    const char* hello = "hello world";
    uint64_t c1 = crc64(hello, 11);
    uint64_t c2 = crc64(hello, 11);
    CHECK(c1 == c2,
          "CRC64 is deterministic -- same input always same output");

    CHECK(crc64("hello World", 11) != c1,
          "CRC64 detects single-byte difference (case change)");

    CHECK(crc64("hello world!", 12) != c1,
          "CRC64 detects added byte");

    CHECK(crc64("hello worlD", 11) != c1,
          "CRC64 detects last-byte change");

    // Incremental update must match one-shot
    uint64_t inc = 0;
    inc = crc64_update(inc, "hello", 5);
    inc = crc64_update(inc, " world", 6);
    CHECK(inc == c1,
          "crc64_update() incremental == one-shot crc64()");

    // Single-byte non-zero inputs give non-zero CRC
    char b = 0x01;
    CHECK(crc64(&b, 1) != 0,
          "CRC64 of byte 0x01 is non-zero");

    // All-zero bytes should give a different CRC than all-one bytes
    uint8_t zeros[8] = {0,0,0,0,0,0,0,0};
    uint8_t ones[8]  = {1,1,1,1,1,1,1,1};
    CHECK(crc64(zeros, 8) != crc64(ones, 8),
          "CRC64 distinguishes all-zero from all-one 8-byte buffer");

    // Three-part split must match two-part split
    uint64_t two   = crc64_update(crc64_update(0ULL, "ab", 2), "cd", 2);
    uint64_t three = crc64_update(crc64_update(crc64_update(0ULL,"a",1),"b",1),"cd",2);
    CHECK(two == three,
          "crc64_update() is associative -- split feeding gives same result");

    // Known property: changing any bit in the middle changes the CRC
    char buf[4] = {0x12, 0x34, 0x56, 0x78};
    uint64_t orig = crc64(buf, 4);
    buf[2] ^= 0x01;
    CHECK(crc64(buf, 4) != orig,
          "CRC64 detects single-bit flip in middle of buffer");
}

// ============================================================
// SECTION 2 -- Type / Encoding converters
// ============================================================
static void test_converters() {
    SECTION("2. Type / Encoding converters (common.cpp)");

    // typeToString
    CHECK(std::string(typeToString(ColumnType::INT32))  == "int32",  "typeToString INT32");
    CHECK(std::string(typeToString(ColumnType::INT64))  == "int64",  "typeToString INT64");
    CHECK(std::string(typeToString(ColumnType::DOUBLE)) == "double", "typeToString DOUBLE");
    CHECK(std::string(typeToString(ColumnType::STRING)) == "string", "typeToString STRING");

    // stringToType round-trips
    CHECK(stringToType("int32")  == ColumnType::INT32,  "stringToType int32");
    CHECK(stringToType("int64")  == ColumnType::INT64,  "stringToType int64");
    CHECK(stringToType("double") == ColumnType::DOUBLE, "stringToType double");
    CHECK(stringToType("string") == ColumnType::STRING, "stringToType string");
    CHECK(stringToType("banana") == ColumnType::STRING, "stringToType unknown -> STRING");
    CHECK(stringToType("")       == ColumnType::STRING, "stringToType empty -> STRING");

    // encodingToString
    CHECK(std::string(encodingToString(Encoding::NONE))       == "none",       "encodingToString NONE");
    CHECK(std::string(encodingToString(Encoding::DICTIONARY)) == "dictionary", "encodingToString DICTIONARY");
    CHECK(std::string(encodingToString(Encoding::RLE))        == "rle",        "encodingToString RLE");

    // stringToEncoding round-trips
    CHECK(stringToEncoding("none")       == Encoding::NONE,       "stringToEncoding none");
    CHECK(stringToEncoding("dictionary") == Encoding::DICTIONARY, "stringToEncoding dictionary");
    CHECK(stringToEncoding("rle")        == Encoding::RLE,        "stringToEncoding rle");
    CHECK(stringToEncoding("unknown")    == Encoding::NONE,       "stringToEncoding unknown -> NONE");
    CHECK(stringToEncoding("")           == Encoding::NONE,       "stringToEncoding empty -> NONE");
}

// ============================================================
// SECTION 3 -- ColumnHeader struct layout (Section 4)
// ============================================================
static void test_header_layout() {
    SECTION("3. ColumnHeader struct layout (Manual Section 4)");

    // Manual specifies exactly 36 bytes, field by field
    CHECK(sizeof(ColumnHeader) == 36,
          "ColumnHeader is exactly 36 bytes (Section 4)");

    // Verify field offsets match the spec table
    ColumnHeader h;
    char* base = reinterpret_cast<char*>(&h);
    CHECK((reinterpret_cast<char*>(&h.magic)     - base) == 0,  "magic at offset 0");
    CHECK((reinterpret_cast<char*>(&h.version)   - base) == 4,  "version at offset 4");
    CHECK((reinterpret_cast<char*>(&h.type)      - base) == 8,  "type at offset 8");
    CHECK((reinterpret_cast<char*>(&h.encoding)  - base) == 9,  "encoding at offset 9");
    CHECK((reinterpret_cast<char*>(&h.reserved)  - base) == 10, "reserved at offset 10");
    CHECK((reinterpret_cast<char*>(&h.row_count) - base) == 12, "row_count at offset 12");
    CHECK((reinterpret_cast<char*>(&h.data_size) - base) == 20, "data_size at offset 20");
    CHECK((reinterpret_cast<char*>(&h.dict_size) - base) == 28, "dict_size at offset 28");

    // Magic bytes must be 'C','O','L','1' = 0x434F4C31
    CHECK(MAGIC == 0x434F4C31u, "MAGIC == 0x434F4C31 ('COL1')");

    // Version must be 1
    CHECK(VERSION == 1u, "VERSION == 1");

    // Size constants
    CHECK(HEADER_SIZE == 36, "HEADER_SIZE == 36");
    CHECK(CRC64_SIZE  == 8,  "CRC64_SIZE == 8");
}

// ============================================================
// SECTION 4 -- TypeInference
// ============================================================
static void test_type_inference() {
    SECTION("4. TypeInference (type_inference.cpp -- Section 5.2 step 1)");

    // isInt64 -- valid
    CHECK(TypeInference::isInt64("0"),                    "isInt64: 0");
    CHECK(TypeInference::isInt64("1"),                    "isInt64: 1");
    CHECK(TypeInference::isInt64("-1"),                   "isInt64: -1");
    CHECK(TypeInference::isInt64("9223372036854775807"),   "isInt64: INT64_MAX");
    CHECK(TypeInference::isInt64("-9223372036854775808"),  "isInt64: INT64_MIN");
    CHECK(TypeInference::isInt64("20240101"),              "isInt64: date-like integer");

    // isInt64 -- invalid
    CHECK(!TypeInference::isInt64(""),       "isInt64: empty -> false");
    CHECK(!TypeInference::isInt64("3.14"),   "isInt64: float -> false");
    CHECK(!TypeInference::isInt64("abc"),    "isInt64: letters -> false");
    CHECK(!TypeInference::isInt64("1e5"),    "isInt64: sci notation -> false");
    CHECK(!TypeInference::isInt64("1 2"),    "isInt64: space -> false");
    CHECK(!TypeInference::isInt64("1.0"),    "isInt64: 1.0 -> false");

    // isDouble -- valid
    CHECK(TypeInference::isDouble("3.14"),    "isDouble: 3.14");
    CHECK(TypeInference::isDouble("1e5"),     "isDouble: 1e5");
    CHECK(TypeInference::isDouble("-0.001"),  "isDouble: -0.001");
    CHECK(TypeInference::isDouble("42"),      "isDouble: integer is valid double");
    CHECK(TypeInference::isDouble("0.0"),     "isDouble: 0.0");

    // isDouble -- invalid
    CHECK(!TypeInference::isDouble(""),      "isDouble: empty -> false");
    CHECK(!TypeInference::isDouble("abc"),   "isDouble: letters -> false");
    CHECK(!TypeInference::isDouble("1 2"),   "isDouble: space -> false");

    // inferType -- INT32 (small values fit in 32-bit range)
    // BUG FIX (Bug 3): type_inference.cpp now returns INT32 for values
    // that fit in INT32_MIN..INT32_MAX range. Tests updated accordingly.
    CHECK(TypeInference::inferType({"1","2","3"})       == ColumnType::INT32,
          "inferType: small integers -> INT32 (fits in 32-bit range)");
    CHECK(TypeInference::inferType({"0","-5","100"})    == ColumnType::INT32,
          "inferType: mixed sign small integers -> INT32");

    // inferType -- INT64 (only for values exceeding INT32 range)
    CHECK(TypeInference::inferType({"9223372036854775807"}) == ColumnType::INT64,
          "inferType: INT64_MAX value -> INT64 (exceeds INT32 range)");
    CHECK(TypeInference::inferType({"-9223372036854775808"}) == ColumnType::INT64,
          "inferType: INT64_MIN value -> INT64 (exceeds INT32 range)");

    // inferType -- DOUBLE
    CHECK(TypeInference::inferType({"1.5","2.5"})       == ColumnType::DOUBLE,
          "inferType: all floats -> DOUBLE");
    CHECK(TypeInference::inferType({"1","2.5","3"})     == ColumnType::DOUBLE,
          "inferType: int+float mix -> DOUBLE");

    // inferType -- STRING
    CHECK(TypeInference::inferType({"a","b","c"})       == ColumnType::STRING,
          "inferType: letters -> STRING");
    CHECK(TypeInference::inferType({"1","hello","3"})   == ColumnType::STRING,
          "inferType: has non-numeric -> STRING");

    // inferType -- empty cell fix (Section 5.2 bug fix)
    CHECK(TypeInference::inferType({"99.9","","14.5"})  == ColumnType::DOUBLE,
          "inferType: empty cells skipped, still DOUBLE");
    CHECK(TypeInference::inferType({"10","","20"})      == ColumnType::INT32,
          "inferType: empty cells skipped, still INT32 (small values)");
    CHECK(TypeInference::inferType({"","",""})          == ColumnType::STRING,
          "inferType: all empty cells -> STRING (explicit all_empty guard in type_inference)");
    CHECK(TypeInference::inferType({})                  == ColumnType::STRING,
          "inferType: empty vector -> STRING");

    // inferAllTypes -- multi-column
    std::vector<Row> rows = {{"1","3.14","hello"},{"2","2.71","world"}};
    auto types = TypeInference::inferAllTypes(rows, 3);
    CHECK(types.size() == 3,                    "inferAllTypes: returns 3 types");
    CHECK(types[0] == ColumnType::INT32,        "inferAllTypes: col0 -> INT32 (small integers)");
    CHECK(types[1] == ColumnType::DOUBLE,       "inferAllTypes: col1 -> DOUBLE");
    CHECK(types[2] == ColumnType::STRING,       "inferAllTypes: col2 -> STRING");

    // inferAllTypes -- empty rows
    auto empty_types = TypeInference::inferAllTypes({}, 3);
    CHECK(empty_types.size() == 3,              "inferAllTypes: empty rows returns 3 defaults");
    CHECK(empty_types[0] == ColumnType::STRING, "inferAllTypes: empty rows -> STRING default");
}

// ============================================================
// SECTION 5 -- CSVParser
// ============================================================
static void test_csv_parser() {
    SECTION("5. CSVParser (csv_parser.cpp -- Section 8)");
    mkdirP(BASE + "/csv");

    // Basic 3-column CSV
    writeFile(BASE+"/csv/basic.csv", "id,name,price\n1,Alice,9.99\n2,Bob,14.50\n3,Charlie,4.00\n");
    {
        CSVParser p(BASE+"/csv/basic.csv");
        CHECK(p.parse(),                    "basic CSV: parse returns true");
        auto h = p.getHeaders();
        CHECK(h.size() == 3,               "basic CSV: 3 headers");
        CHECK(h[0] == "id",                "basic CSV: header[0] == id");
        CHECK(h[1] == "name",              "basic CSV: header[1] == name");
        CHECK(h[2] == "price",             "basic CSV: header[2] == price");
        CHECK(p.getRowCount() == 3,        "basic CSV: 3 data rows");
        auto rows = p.getRows();
        CHECK(rows[0][0] == "1",           "basic CSV: row0[0] == 1");
        CHECK(rows[0][2] == "9.99",        "basic CSV: row0[2] == 9.99");
        CHECK(rows[2][1] == "Charlie",     "basic CSV: row2[1] == Charlie");
    }

    // Quoted field with embedded comma
    writeFile(BASE+"/csv/quoted.csv", "id,city\n1,\"New York, NY\"\n2,London\n");
    {
        CSVParser p(BASE+"/csv/quoted.csv");
        CHECK(p.parse(),                              "quoted CSV: parse returns true");
        CHECK(p.getRows()[0][1] == "New York, NY",   "quoted CSV: embedded comma preserved");
        CHECK(p.getRows()[1][1] == "London",          "quoted CSV: unquoted field ok");
    }

    // Escaped double-quote inside quoted field
    writeFile(BASE+"/csv/dquote.csv", "id,value\n1,\"say \"\"hello\"\"\"\n");
    {
        CSVParser p(BASE+"/csv/dquote.csv");
        CHECK(p.parse(),                               "double-quote CSV: parse ok");
        CHECK(p.getRows()[0][1] == "say \"hello\"",   "double-quote: decoded correctly");
    }

    // Empty fields
    writeFile(BASE+"/csv/empty.csv", "a,b,c\n1,,3\n,2,\n");
    {
        CSVParser p(BASE+"/csv/empty.csv");
        CHECK(p.parse(),                     "empty-field CSV: parse ok");
        auto rows = p.getRows();
        CHECK(rows[0][1] == "",              "empty field: middle is empty string");
        CHECK(rows[1][0] == "",              "empty field: leading is empty string");
        CHECK(rows[1][2] == "",              "empty field: trailing is empty string");
    }

    // Windows CRLF line endings
    writeFile(BASE+"/csv/crlf.csv", "x,y\r\n10,20\r\n30,40\r\n");
    {
        CSVParser p(BASE+"/csv/crlf.csv");
        CHECK(p.parse(),                          "CRLF CSV: parse ok");
        auto rows = p.getRows();
        CHECK(rows[0][0] == "10",                 "CRLF: row0[0] == 10");
        CHECK(rows[0][1] == "20",                 "CRLF: row0[1] == 20");
        CHECK(rows[1][0] == "30",                 "CRLF: row1[0] == 30");
    }

    // Single column CSV
    writeFile(BASE+"/csv/single.csv", "value\n42\n99\n7\n");
    {
        CSVParser p(BASE+"/csv/single.csv");
        CHECK(p.parse(),                  "single-col CSV: parse ok");
        CHECK(p.getHeaders().size() == 1, "single-col CSV: 1 header");
        CHECK(p.getRowCount() == 3,       "single-col CSV: 3 rows");
    }

    // Many columns
    writeFile(BASE+"/csv/wide.csv", "a,b,c,d,e,f,g\n1,2,3,4,5,6,7\n");
    {
        CSVParser p(BASE+"/csv/wide.csv");
        CHECK(p.parse(),                   "wide CSV: parse ok");
        CHECK(p.getHeaders().size() == 7,  "wide CSV: 7 headers");
        CHECK(p.getRows()[0][6] == "7",    "wide CSV: last column value ok");
    }

    // Empty file (header only)
    writeFile(BASE+"/csv/header_only.csv", "id,name\n");
    {
        CSVParser p(BASE+"/csv/header_only.csv");
        CHECK(p.parse(),             "header-only CSV: parse ok");
        CHECK(p.getRowCount() == 0,  "header-only CSV: 0 data rows");
    }

    // Non-existent file
    {
        CSVParser p(BASE+"/csv/does_not_exist.csv");
        CHECK(!p.parse(),                   "non-existent: parse returns false");
        CHECK(!p.getLastError().empty(),    "non-existent: error message set");
    }
}

// ============================================================
// SECTION 6 -- ColumnWriter + ColumnReader (NONE encoding)
// ============================================================
static void test_writer_reader_none() {
    SECTION("6. ColumnWriter + ColumnReader NONE encoding (Phase 1)");
    const std::string D = BASE + "/none";
    rmDir(D); mkdirP(D);

    // INT64 roundtrip
    // DIAGNOSTIC: verify the directory was actually created before writing
    {
        std::error_code ec2;
        bool dir_exists = std::filesystem::exists(D, ec2);
        CHECK(dir_exists, "INT64 pre-check: directory colsh_test_tmp/none exists");
        if (!dir_exists) {
            // Try to create it explicitly and report
            std::filesystem::create_directories(D, ec2);
            CHECK(!ec2, std::string("INT64 pre-check: mkdir error: ") + ec2.message());
        }
    }
    {
        std::vector<int64_t> vals = {0, 1, -1, 42, INT64_MAX, INT64_MIN, 99999};
        ColumnWriter w(D,"int64",ColumnType::INT64,Encoding::NONE);
        w.setRowCount(vals.size());
        CHECK(w.writeInt64(vals),  "INT64: writeInt64 returns true");
        CHECK(w.finalize(),         "INT64: finalize returns true");

        // .col.tmp must be gone, .col must exist
        CHECK(fileSize(D+"/int64.col")     > 0,  "INT64: .col file exists");
        CHECK(fileSize(D+"/int64.col.tmp") == -1, "INT64: .col.tmp removed");

        // File size: 36 (header) + 7*8 (data) + 8 (CRC) = 100 bytes
        CHECK(fileSize(D+"/int64.col") == 100, "INT64: file size == 36+56+8=100 bytes");

        ColumnReader r;
        CHECK(r.open(D,"int64"),                    "INT64 reader: open ok");
        CHECK(r.isOpen(),                            "INT64 reader: isOpen true");
        CHECK(r.getType()     == ColumnType::INT64,  "INT64 reader: type correct");
        CHECK(r.getEncoding() == Encoding::NONE,     "INT64 reader: encoding NONE");
        CHECK(r.getRowCount() == vals.size(),        "INT64 reader: row_count correct");

        auto got = readAll(r);
        CHECK(got.size() == vals.size(),             "INT64 reader: read N rows");
        bool ok = (got.size() == vals.size());
        for (size_t i = 0; i < got.size(); i++)
            if (std::get<int64_t>(got[i]) != vals[i]) ok = false;
        CHECK(ok, "INT64 reader: all values match");

        // reset - guard r.next() so a failed open doesn't crash
        r.reset();
        bool has = r.hasNext();
        CHECK(has, "INT64 reader: hasNext after reset");
        CHECK(has && std::get<int64_t>(r.next()) == vals[0],"INT64 reader: first value after reset");
        r.close();
        CHECK(!r.isOpen(),                           "INT64 reader: isOpen false after close");
    }

    // DOUBLE roundtrip
    {
        std::vector<double> vals = {0.0, 1.5, -3.14, 1e10, -1e-10, 99.99};
        ColumnWriter w(D,"dbl",ColumnType::DOUBLE,Encoding::NONE);
        w.setRowCount(vals.size());
        CHECK(w.writeDouble(vals), "DOUBLE: write ok");
        CHECK(w.finalize(),         "DOUBLE: finalize ok");

        // 36 + 6*8 + 8 = 92
        CHECK(fileSize(D+"/dbl.col") == 92, "DOUBLE: file size == 36+48+8=92 bytes");

        ColumnReader r;
        CHECK(r.open(D,"dbl"),                      "DOUBLE reader: open ok");
        CHECK(r.getType() == ColumnType::DOUBLE,     "DOUBLE reader: type DOUBLE");
        auto got = readAll(r);
        bool ok = (got.size() == vals.size());
        for (size_t i = 0; i < got.size(); i++)
            if (std::get<double>(got[i]) != vals[i]) ok = false;
        CHECK(ok, "DOUBLE reader: all values match exactly");
        r.close();
    }

    // STRING roundtrip (2-byte length prefix)
    {
        std::vector<std::string> vals = {"hello","world","","United States","a","with,comma"};
        ColumnWriter w(D,"str",ColumnType::STRING,Encoding::NONE);
        w.setRowCount(vals.size());
        CHECK(w.writeString(vals), "STRING: write ok");
        CHECK(w.finalize(),         "STRING: finalize ok");

        ColumnReader r;
        CHECK(r.open(D,"str"),                      "STRING reader: open ok");
        CHECK(r.getType() == ColumnType::STRING,     "STRING reader: type STRING");
        CHECK(r.getRowCount() == vals.size(),        "STRING reader: row_count correct");

        auto got = readAll(r);
        bool ok = (got.size() == vals.size());
        for (size_t i = 0; i < got.size(); i++)
            if (std::get<std::string>(got[i]) != vals[i]) ok = false;
        CHECK(ok, "STRING reader: all values match (incl empty and comma)");
        r.close();
    }

    // Single-row edge case
    {
        ColumnWriter w(D,"one",ColumnType::INT64,Encoding::NONE);
        w.setRowCount(1);
        CHECK(w.writeInt64({42}), "single-row: write ok");
        CHECK(w.finalize(),        "single-row: finalize ok");

        ColumnReader r;
        CHECK(r.open(D,"one"),                  "single-row reader: open ok");
        CHECK(r.getRowCount() == 1,             "single-row reader: row_count 1");
        CHECK(r.hasNext(),                       "single-row reader: hasNext true");
        CHECK(std::get<int64_t>(r.next()) == 42,"single-row reader: value 42");
        CHECK(!r.hasNext(),                      "single-row reader: hasNext false after read");
        r.close();
    }

    // Zero-row edge case
    {
        ColumnWriter w(D,"zero",ColumnType::INT64,Encoding::NONE);
        w.setRowCount(0);
        CHECK(w.writeInt64({}), "zero-row: write ok");
        CHECK(w.finalize(),      "zero-row: finalize ok");

        ColumnReader r;
        CHECK(r.open(D,"zero"),          "zero-row reader: open ok");
        CHECK(r.getRowCount() == 0,      "zero-row reader: row_count 0");
        CHECK(!r.hasNext(),              "zero-row reader: hasNext false immediately");
        r.close();
    }

    // CRC corruption detection (Manual Section 4)
    {
        ColumnWriter w(D,"crc",ColumnType::INT64,Encoding::NONE);
        w.setRowCount(3);
        w.writeInt64({10,20,30});
        w.finalize();

        // Flip a byte in the data section
        std::fstream f(D+"/crc.col", std::ios::binary|std::ios::in|std::ios::out);
        f.seekp(40);
        char bad = 0xFF; f.write(&bad,1); f.close();

        ColumnReader r;
        CHECK(!r.open(D,"crc"),             "CRC corruption: open returns false");
        CHECK(!r.getLastError().empty(),    "CRC corruption: error message set");
    }

    rmDir(D);
}

// ============================================================
// SECTION 7 -- DICTIONARY encoding (Phase 2)
// ============================================================
static void test_dictionary() {
    SECTION("7. DICTIONARY encoding (Phase 2 -- Section 5.1 + 6.1)");
    const std::string D = BASE + "/dict";
    rmDir(D); mkdirP(D);

    // Basic roundtrip with 3 distinct values
    {
        std::vector<std::string> vals = {
            "Electronics","Books","Electronics","Clothing",
            "Books","Electronics","Clothing","Books"
        };
        ColumnWriter w(D,"cat",ColumnType::STRING,Encoding::DICTIONARY);
        w.setRowCount(vals.size());
        CHECK(w.writeDictionary(vals), "dict basic: writeDictionary ok");
        CHECK(w.finalize(),             "dict basic: finalize ok");

        ColumnReader r;
        CHECK(r.open(D,"cat"),                          "dict basic reader: open ok");
        CHECK(r.getEncoding() == Encoding::DICTIONARY,  "dict basic: encoding==DICTIONARY");
        CHECK(r.getRowCount() == vals.size(),           "dict basic: row_count correct");

        auto got = readAll(r);
        CHECK(got.size() == vals.size(),                "dict basic: read N values");
        bool ok = true;
        for (size_t i=0; i<vals.size(); i++)
            if (std::get<std::string>(got[i]) != vals[i]) ok=false;
        CHECK(ok, "dict basic: all values decode correctly");
        r.close();
    }

    // Dictionary must be smaller than raw for low-cardinality string column
    {
        std::vector<std::string> vals(10000, "United States");
        for (size_t i=0; i<vals.size(); i++)
            vals[i] = (i % 24 == 0) ? "Germany" : (i % 3 == 0) ? "UK" : "United States";

        long raw_size  = writeStringCol(D,"raw_cmp",  vals, Encoding::NONE);
        long dict_size = writeStringCol(D,"dict_cmp", vals, Encoding::DICTIONARY);
        CHECK(dict_size < raw_size,
              "dict compression: DICTIONARY smaller than raw for low-cardinality column");
    }

    // dict_size field in header must be non-zero for DICTIONARY columns
    {
        std::vector<std::string> vals = {"A","B","A","C"};
        ColumnWriter w(D,"dsz",ColumnType::STRING,Encoding::DICTIONARY);
        w.setRowCount(vals.size()); w.writeDictionary(vals); w.finalize();

        // Read header directly
        std::ifstream f(D+"/dsz.col", std::ios::binary);
        ColumnHeader h; f.read(reinterpret_cast<char*>(&h), sizeof(h));
        CHECK(h.dict_size > 0,    "dict header: dict_size > 0 for DICTIONARY column");
        CHECK(h.data_size > 0,    "dict header: data_size > 0");
        CHECK(h.row_count == 4,   "dict header: row_count == 4");
    }

    // reset() works correctly for DICTIONARY column
    {
        std::vector<std::string> vals = {"X","Y","X","Z"};
        ColumnWriter w(D,"rst",ColumnType::STRING,Encoding::DICTIONARY);
        w.setRowCount(vals.size()); w.writeDictionary(vals); w.finalize();

        ColumnReader r;
        r.open(D,"rst");
        readAll(r);
        r.reset();
        CHECK(r.hasNext(),                               "dict reset: hasNext true after reset");
        CHECK(std::get<std::string>(r.next()) == "X",   "dict reset: first value correct");
        r.close();
    }

    // Single distinct value (D=1) -- id_width should be 1 byte
    {
        std::vector<std::string> vals(100, "only_one");
        ColumnWriter w(D,"one_val",ColumnType::STRING,Encoding::DICTIONARY);
        w.setRowCount(vals.size()); w.writeDictionary(vals); w.finalize();

        ColumnReader r;
        r.open(D,"one_val");
        CHECK(r.getEncoding() == Encoding::DICTIONARY, "dict D=1: encoding DICTIONARY");
        auto got = readAll(r);
        bool ok = true;
        for (auto& v : got) if (std::get<std::string>(v) != "only_one") ok=false;
        CHECK(ok, "dict D=1: all values decode to only_one");
        r.close();
    }

    // Exactly 256 distinct values (boundary: D=256, id_width=1)
    {
        std::vector<std::string> vals;
        for (int i=0; i<256; i++) vals.push_back("val_" + std::to_string(i));
        // Repeat each 3 times
        std::vector<std::string> repeated;
        for (int r=0; r<3; r++) for (auto& v : vals) repeated.push_back(v);

        ColumnWriter w(D,"d256",ColumnType::STRING,Encoding::DICTIONARY);
        w.setRowCount(repeated.size()); w.writeDictionary(repeated); w.finalize();

        ColumnReader rdr;
        rdr.open(D,"d256");
        CHECK(rdr.getEncoding() == Encoding::DICTIONARY, "dict D=256: encoding DICTIONARY");
        auto got = readAll(rdr);
        bool ok = (got.size() == repeated.size());
        for (size_t i=0; i<got.size() && ok; i++)
            if (std::get<std::string>(got[i]) != repeated[i]) ok=false;
        CHECK(ok, "dict D=256: all 768 values decode correctly");
        rdr.close();
    }

    // D > 65535 must fall back to NONE (Manual Section 5.1)
    {
        std::vector<std::string> high;
        for (int i=0; i<70000; i++) high.push_back("s_" + std::to_string(i));

        ColumnWriter w(D,"hc",ColumnType::STRING,Encoding::DICTIONARY);
        w.setRowCount(high.size()); w.writeDictionary(high); w.finalize();

        ColumnReader r;
        r.open(D,"hc");
        CHECK(r.getEncoding() == Encoding::NONE,
              "dict fallback: D>65535 falls back to NONE encoding");

        // Values should still be readable
        auto got = readAll(r);
        CHECK(got.size() == high.size(),          "dict fallback: all values readable");
        CHECK(std::get<std::string>(got[0]) == high[0], "dict fallback: first value correct");
        r.close();
    }

    // Empty string in values handled correctly
    {
        std::vector<std::string> vals = {"","hello","","world",""};
        ColumnWriter w(D,"empty_str",ColumnType::STRING,Encoding::DICTIONARY);
        w.setRowCount(vals.size()); w.writeDictionary(vals); w.finalize();

        ColumnReader r;
        r.open(D,"empty_str");
        auto got = readAll(r);
        bool ok = true;
        for (size_t i=0; i<vals.size(); i++)
            if (std::get<std::string>(got[i]) != vals[i]) ok=false;
        CHECK(ok, "dict empty strings: empty string values preserved");
        r.close();
    }

    rmDir(D);
}

// ============================================================
// SECTION 8 -- RLE encoding (Phase 2)
// ============================================================
static void test_rle() {
    SECTION("8. RLE encoding (Phase 2 -- Section 5.1 + 6.1)");
    const std::string D = BASE + "/rle";
    rmDir(D); mkdirP(D);

    // INT64 roundtrip -- sorted dates pattern (high compression)
    {
        std::vector<int64_t> dates;
        for (int d=1; d<=10; d++)
            for (int r=0; r<1000; r++)
                dates.push_back(d * 100LL);  // 10000 rows, 10 distinct

        ColumnWriter w(D,"date",ColumnType::INT64,Encoding::RLE);
        w.setRowCount(dates.size());
        CHECK(w.writeRLE_Int64(dates), "RLE INT64: writeRLE_Int64 ok");
        CHECK(w.finalize(),             "RLE INT64: finalize ok");

        ColumnReader r;
        CHECK(r.open(D,"date"),              "RLE INT64 reader: open ok");
        CHECK(r.getEncoding() == Encoding::RLE, "RLE INT64: encoding==RLE");
        CHECK(r.getRowCount() == dates.size(),"RLE INT64: row_count correct");

        auto got = readAll(r);
        CHECK(got.size() == dates.size(),    "RLE INT64: read all 10000 rows");
        bool ok = true;
        for (size_t i=0; i<dates.size(); i++)
            if (std::get<int64_t>(got[i]) != dates[i]) ok=false;
        CHECK(ok, "RLE INT64: all values decode correctly");
        r.close();
    }

    // RLE file must be smaller than raw for sorted data
    {
        std::vector<int64_t> sorted;
        for (int d=1; d<=365; d++)
            for (int r=0; r<1000; r++)
                sorted.push_back(20240000LL + d);  // 365000 rows

        long rle_sz  = writeInt64Col(D,"rle_sz",  sorted, Encoding::RLE);
        long none_sz = writeInt64Col(D,"none_sz", sorted, Encoding::NONE);
        CHECK(rle_sz < none_sz,
              "RLE compression: sorted data RLE file is smaller than raw");
    }

    // RLE DOUBLE roundtrip
    {
        std::vector<double> dvals;
        for (int v=1; v<=5; v++)
            for (int r=0; r<1000; r++)
                dvals.push_back(v * 1.5);

        ColumnWriter w(D,"dbl_rle",ColumnType::DOUBLE,Encoding::RLE);
        w.setRowCount(dvals.size());
        CHECK(w.writeRLE_Double(dvals), "RLE DOUBLE: write ok");
        CHECK(w.finalize(),              "RLE DOUBLE: finalize ok");

        ColumnReader r;
        r.open(D,"dbl_rle");
        CHECK(r.getEncoding() == Encoding::RLE, "RLE DOUBLE: encoding==RLE");
        auto got = readAll(r);
        bool ok = true;
        for (size_t i=0; i<dvals.size(); i++)
            if (std::get<double>(got[i]) != dvals[i]) ok=false;
        CHECK(ok, "RLE DOUBLE: all values decode correctly");
        r.close();
    }

    // RLE reset() must replay from start
    {
        std::vector<int64_t> vals;
        for (int v=1; v<=3; v++)
            for (int r=0; r<500; r++)
                vals.push_back(v);

        ColumnWriter w(D,"rle_rst",ColumnType::INT64,Encoding::RLE);
        w.setRowCount(vals.size()); w.writeRLE_Int64(vals); w.finalize();

        ColumnReader r;
        r.open(D,"rle_rst");
        readAll(r);
        r.reset();
        CHECK(r.hasNext(),                            "RLE reset: hasNext after reset");
        CHECK(std::get<int64_t>(r.next()) == vals[0],"RLE reset: first value correct after reset");
        r.close();
    }

    // RLE fallback: all-unique values → run_count = N, N/4 < N → falls back to NONE
    // Manual Section 5.1: "only choose RLE if run_count < N/4"
    {
        std::vector<int64_t> unique;
        for (int64_t i=0; i<1000; i++) unique.push_back(i);

        ColumnWriter w(D,"fallback",ColumnType::INT64,Encoding::RLE);
        w.setRowCount(unique.size()); w.writeRLE_Int64(unique); w.finalize();

        ColumnReader r;
        r.open(D,"fallback");
        CHECK(r.getEncoding() == Encoding::NONE,
              "RLE fallback: all-unique data falls back to NONE");
        auto got = readAll(r);
        CHECK(got.size() == unique.size(),         "RLE fallback: all values still readable");
        CHECK(std::get<int64_t>(got[0]) == 0,      "RLE fallback: first value correct");
        CHECK(std::get<int64_t>(got[999]) == 999,  "RLE fallback: last value correct");
        r.close();
    }

    // RLE: single run (all same value)
    {
        std::vector<int64_t> same(5000, 42LL);
        ColumnWriter w(D,"same",ColumnType::INT64,Encoding::RLE);
        w.setRowCount(same.size()); w.writeRLE_Int64(same); w.finalize();

        ColumnReader r;
        r.open(D,"same");
        CHECK(r.getEncoding() == Encoding::RLE, "RLE single run: encoding==RLE");
        auto got = readAll(r);
        bool ok = true;
        for (auto& v : got) if (std::get<int64_t>(v) != 42) ok=false;
        CHECK(ok, "RLE single run: all 5000 values == 42");
        r.close();
    }

    // RLE: exactly 2 runs
    {
        std::vector<int64_t> two_runs(2000, 10LL);
        for (int i=1000; i<2000; i++) two_runs[i] = 20LL;

        ColumnWriter w(D,"two",ColumnType::INT64,Encoding::RLE);
        w.setRowCount(two_runs.size()); w.writeRLE_Int64(two_runs); w.finalize();

        ColumnReader r;
        r.open(D,"two");
        CHECK(r.getEncoding() == Encoding::RLE, "RLE 2-run: encoding==RLE");
        auto got = readAll(r);
        CHECK(std::get<int64_t>(got[0])    == 10, "RLE 2-run: first half is 10");
        CHECK(std::get<int64_t>(got[1000]) == 20, "RLE 2-run: second half is 20");
        r.close();
    }

    rmDir(D);
}

// ============================================================
// SECTION 9 -- Encoding selection via Loader (Phase 2)
// ============================================================
static void test_encoding_selection() {
    SECTION("9. Encoding selection logic (loader chooseEncoding -- Section 5.1)");
    const std::string WH = BASE + "/enc_sel_wh";
    rmDir(WH); mkdirP(WH);

    // Write CSV with: string col (low cardinality), sorted int col, random int col
    // String col should get DICTIONARY
    // Sorted int (many repeats) should get RLE
    // Random int (high cardinality) should get NONE

    // Build CSV: id (sequential), date (sorted with many repeats), country (24 distinct)
    std::string csv = BASE + "/enc_test.csv";
    {
        std::ofstream f(csv);
        f << "id,date,country\n";
        const char* countries[] = {
            "US","UK","DE","FR","JP","CN","IN","BR","AU","CA",
            "IT","ES","MX","KR","RU","NL","SE","PL","TR","SA",
            "ZA","NG","AR","EG"
        };
        for (int i=0; i<10000; i++) {
            // id: sequential (no runs), date: sorted (lots of runs), country: 24 distinct
            int date = 20240101 + (i / 100);  // each date repeats 100 times
            f << i << "," << date << "," << countries[i % 24] << "\n";
        }
    }

    CHECK(Loader::load(csv, "enc_test", WH), "encoding selection: loader succeeds");

    // Check schema was written
    std::string tdir = WH + "/enc_test";
    CHECK(SchemaManager::schemaExists(tdir), "encoding selection: schema.json written");

    auto schema = SchemaManager::readSchema(tdir);
    CHECK(schema.size() == 3, "encoding selection: schema has 3 columns");

    // id: sequential INT64, no runs (run_count == N-1) -> NONE
    std::string id_enc = "", date_enc = "", country_enc = "";
    for (auto& sc : schema) {
        if (sc.name == "id")      id_enc      = encodingToString(sc.encoding);
        if (sc.name == "date")    date_enc    = encodingToString(sc.encoding);
        if (sc.name == "country") country_enc = encodingToString(sc.encoding);
    }

    CHECK(id_enc == "none",
          "encoding selection: sequential id -> NONE (no runs)");
    CHECK(date_enc == "rle",
          "encoding selection: sorted date col (100x repeats) -> RLE");
    CHECK(country_enc == "dictionary",
          "encoding selection: low-cardinality string -> DICTIONARY");

    // Verify columns are actually readable with their chosen encoding
    {
        ColumnReader r;
        CHECK(r.open(tdir,"country"), "enc sel: country column opens with DICTIONARY");
        r.close();
    }
    {
        ColumnReader r;
        CHECK(r.open(tdir,"date"), "enc sel: date column opens with RLE");
        r.close();
    }

    rmDir(WH);
    std::remove(csv.c_str());
}

// ============================================================
// SECTION 10 -- SchemaManager
// ============================================================
static void test_schema_manager() {
    SECTION("10. SchemaManager (schema.cpp -- Section 5.2 step 6)");
    const std::string D = BASE + "/schema";
    rmDir(D); mkdirP(D);

    // schemaExists before write
    CHECK(!SchemaManager::schemaExists(D), "schema: schemaExists false before write");

    // Write schema with all types and encodings
    std::vector<SchemaColumn> cols = {
        {"id",      ColumnType::INT64,  Encoding::NONE},
        {"price",   ColumnType::DOUBLE, Encoding::NONE},
        {"country", ColumnType::STRING, Encoding::DICTIONARY},
        {"date",    ColumnType::INT64,  Encoding::RLE},
    };
    CHECK(SchemaManager::writeSchema(D, cols, "test"), "schema: writeSchema returns true");

    // Temp file must be removed (temp-and-rename pattern)
    CHECK(fileSize(D+"/schema.json.tmp") == -1, "schema: .tmp removed after rename");
    CHECK(fileSize(D+"/schema.json")     >  0,  "schema: schema.json exists");

    CHECK(SchemaManager::schemaExists(D), "schema: schemaExists true after write");

    // Read back and verify all fields
    auto rc = SchemaManager::readSchema(D);
    CHECK(rc.size() == 4,                          "schema readback: 4 columns");
    CHECK(rc[0].name == "id",                      "schema readback: col0 name");
    CHECK(rc[0].type == ColumnType::INT64,         "schema readback: col0 type INT64");
    CHECK(rc[0].encoding == Encoding::NONE,        "schema readback: col0 encoding NONE");
    CHECK(rc[1].name == "price",                   "schema readback: col1 name");
    CHECK(rc[1].type == ColumnType::DOUBLE,        "schema readback: col1 type DOUBLE");
    CHECK(rc[2].name == "country",                 "schema readback: col2 name");
    CHECK(rc[2].encoding == Encoding::DICTIONARY,  "schema readback: col2 encoding DICTIONARY");
    CHECK(rc[3].name == "date",                    "schema readback: col3 name");
    CHECK(rc[3].encoding == Encoding::RLE,         "schema readback: col3 encoding RLE");

    // Overwrite schema (tests Windows rename fix)
    std::vector<SchemaColumn> cols2 = {{"x", ColumnType::INT64, Encoding::NONE}};
    CHECK(SchemaManager::writeSchema(D, cols2, "test"), "schema: overwrite returns true");
    auto rc2 = SchemaManager::readSchema(D);
    CHECK(rc2.size() == 1,         "schema overwrite: 1 column");
    CHECK(rc2[0].name == "x",      "schema overwrite: name == x");

    // Non-existent directory returns empty
    auto empty = SchemaManager::readSchema(BASE + "/does_not_exist_xyz");
    CHECK(empty.empty(), "schema: non-existent dir returns empty vector");

    // schemaExists returns false for non-existent
    CHECK(!SchemaManager::schemaExists(BASE + "/nope"), "schema: schemaExists false for missing");

    rmDir(D);
}

// ============================================================
// SECTION 11 -- QueryParser
// ============================================================
static void test_query_parser() {
    SECTION("11. QueryParser (query_parser.cpp -- Section 7)");

    auto parse = [](const std::string& q, QueryPlan& plan) -> bool {
        QueryParser p(q);
        return p.parse(plan);
    };

    // COUNT(*) FROM
    {
        QueryPlan p; CHECK(parse("SELECT COUNT(*) FROM sales", p), "parse COUNT(*) FROM");
        CHECK(p.table == "sales",                 "COUNT(*): table");
        CHECK(p.selects.size() == 1,              "COUNT(*): 1 expr");
        CHECK(p.selects[0].agg == AggFunc::COUNT, "COUNT(*): agg COUNT");
        CHECK(p.selects[0].star == true,          "COUNT(*): star true");
        CHECK(!p.where.has_where,                 "COUNT(*): no WHERE");
        CHECK(p.groupby.empty(),                  "COUNT(*): no GROUP BY");
        CHECK(!p.select_star,                     "COUNT(*): select_star false");
    }

    // SUM with WHERE
    {
        QueryPlan p;
        CHECK(parse("SELECT SUM(price) FROM sales WHERE date >= 20240101", p), "parse SUM WHERE");
        CHECK(p.selects[0].agg == AggFunc::SUM,   "SUM WHERE: agg SUM");
        CHECK(p.selects[0].col == "price",         "SUM WHERE: col price");
        CHECK(p.where.has_where,                   "SUM WHERE: has_where");
        CHECK(p.where.col == "date",               "SUM WHERE: where col date");
        CHECK(p.where.op  == ">=",                 "SUM WHERE: where op >=");
        CHECK(p.where.val == "20240101",            "SUM WHERE: where val");
    }

    // SELECT *
    {
        QueryPlan p; CHECK(parse("SELECT * FROM inventory", p), "parse SELECT *");
        CHECK(p.select_star,              "SELECT *: select_star true");
        CHECK(p.table == "inventory",     "SELECT *: table");
        CHECK(p.selects.empty(),          "SELECT *: selects empty");
    }

    // Multi-column SELECT
    {
        QueryPlan p;
        CHECK(parse("SELECT id, name FROM users", p), "parse multi-col SELECT");
        CHECK(p.selects.size() == 2,           "multi-col: 2 exprs");
        CHECK(p.selects[0].col == "id",         "multi-col: col0 id");
        CHECK(p.selects[1].col == "name",       "multi-col: col1 name");
        CHECK(p.selects[0].agg == AggFunc::NONE,"multi-col: no agg");
    }

    // GROUP BY
    {
        QueryPlan p;
        CHECK(parse("SELECT country, SUM(price) FROM sales GROUP BY country", p), "parse GROUP BY");
        CHECK(p.groupby == "country",  "GROUP BY: groupby col");
    }

    // All six comparison operators
    for (const auto& op : std::vector<std::string>{"=","!=","<","<=",">",">="}) {
        QueryPlan p;
        CHECK(parse("SELECT x FROM t WHERE y " + op + " 5", p) && p.where.op == op,
              "WHERE op: " + op);
    }

    // String literal in WHERE (quotes stripped)
    {
        QueryPlan p;
        CHECK(parse("SELECT x FROM t WHERE cat = 'Electronics'", p), "string literal WHERE");
        CHECK(p.where.val == "Electronics", "string literal: quotes stripped");
    }

    // AVG, MIN, MAX
    {
        QueryPlan p;
        CHECK(parse("SELECT AVG(qty), MIN(price), MAX(price) FROM t", p), "parse AVG MIN MAX");
        CHECK(p.selects[0].agg == AggFunc::AVG, "AVG parsed");
        CHECK(p.selects[1].agg == AggFunc::MIN, "MIN parsed");
        CHECK(p.selects[2].agg == AggFunc::MAX, "MAX parsed");
    }

    // Case insensitive keywords
    {
        QueryPlan p;
        CHECK(parse("select sum(price) from sales where date >= 1", p), "lowercase keywords");
        CHECK(p.table == "sales",               "lowercase: table correct");
        CHECK(p.selects[0].agg == AggFunc::SUM, "lowercase: SUM parsed");
    }

    // neededColumns() -- deduplication
    {
        QueryPlan p;
        parse("SELECT country, SUM(price) FROM sales WHERE date >= 1", p);
        auto needed = p.neededColumns();
        CHECK(needed.size() == 3, "neededColumns: 3 unique cols (country,price,date)");

        // Same col in SELECT and WHERE -- no duplicate
        QueryPlan p2;
        parse("SELECT price FROM t WHERE price > 5", p2);
        CHECK(p2.neededColumns().size() == 1, "neededColumns: no duplicate for price");
    }

    // SELECT * neededColumns returns empty (executor opens all schema cols)
    {
        QueryPlan p;
        parse("SELECT * FROM t", p);
        CHECK(p.neededColumns().empty(), "neededColumns: empty for SELECT *");
    }

    // Syntax errors
    {
        QueryPlan p;
        QueryParser p1("FROM t");   CHECK(!p1.parse(p), "parse error: missing SELECT");
        QueryParser p2("SELECT x"); CHECK(!p2.parse(p), "parse error: missing FROM");
        QueryParser p3("");         CHECK(!p3.parse(p), "parse error: empty query");
    }
}

// ============================================================
// SECTION 12 -- PredicateEvaluator
// ============================================================
static void test_predicate() {
    SECTION("12. PredicateEvaluator (predicate.cpp -- Section 6.2)");
    const std::string D = BASE + "/pred";
    rmDir(D); mkdirP(D);

    // Build INT64 column: [10,20,30,40,50]
    {
        ColumnWriter w(D,"val",ColumnType::INT64,Encoding::NONE);
        w.setRowCount(5); w.writeInt64({10,20,30,40,50}); w.finalize();
    }

    auto eval = [&](const std::string& op, const std::string& lit) -> Bitmap {
        WherePredicate pred; pred.has_where=true; pred.col="val"; pred.op=op; pred.val=lit;
        return PredicateEvaluator::evaluate(pred, D, 5);
    };

    // = 30 -> only row 2
    auto bm = eval("=","30");
    CHECK(bm.size()==5,                              "pred =: bitmap size 5");
    CHECK(!bm[0]&&!bm[1]&&bm[2]&&!bm[3]&&!bm[4],  "pred =: only row 2 passes");
    CHECK(PredicateEvaluator::countTrue(bm)==1,      "pred =: countTrue 1");

    // != 30 -> 4 rows
    CHECK(PredicateEvaluator::countTrue(eval("!=","30"))==4, "pred !=: 4 rows");

    // < 30 -> rows 0,1
    auto bm_lt = eval("<","30");
    CHECK(PredicateEvaluator::countTrue(bm_lt)==2,           "pred <: 2 rows");
    CHECK(bm_lt[0] && bm_lt[1] && !bm_lt[2],                "pred <: rows 0,1 pass");

    // <= 30 -> rows 0,1,2
    CHECK(PredicateEvaluator::countTrue(eval("<=","30"))==3, "pred <=: 3 rows");

    // > 30 -> rows 3,4
    auto bm_gt = eval(">","30");
    CHECK(PredicateEvaluator::countTrue(bm_gt)==2,           "pred >: 2 rows");
    CHECK(bm_gt[3] && bm_gt[4],                              "pred >: rows 3,4 pass");

    // >= 30 -> rows 2,3,4
    CHECK(PredicateEvaluator::countTrue(eval(">=","30"))==3, "pred >=: 3 rows");

    // No WHERE -> all pass
    {
        WherePredicate nw; nw.has_where=false;
        auto all = PredicateEvaluator::evaluate(nw, D, 5);
        CHECK(PredicateEvaluator::countTrue(all)==5, "no WHERE: all 5 pass");
    }

    // Value matches nothing
    CHECK(PredicateEvaluator::countTrue(eval("=","999"))==0, "pred =999: 0 rows");

    // String column predicate
    {
        ColumnWriter w(D,"cat",ColumnType::STRING,Encoding::NONE);
        w.setRowCount(4); w.writeString({"Electronics","Books","Electronics","Clothing"}); w.finalize();

        WherePredicate pred; pred.has_where=true; pred.col="cat"; pred.op="="; pred.val="Electronics";
        auto bm2 = PredicateEvaluator::evaluate(pred, D, 4);
        CHECK(PredicateEvaluator::countTrue(bm2)==2,       "string pred =: 2 matches");
        CHECK(bm2[0]&&!bm2[1]&&bm2[2]&&!bm2[3],          "string pred =: rows 0,2 pass");
    }

    // DICTIONARY column predicate -- must work through reader abstraction
    {
        ColumnWriter w(D,"dict_cat",ColumnType::STRING,Encoding::DICTIONARY);
        w.setRowCount(4); w.writeDictionary({"Electronics","Books","Electronics","Clothing"}); w.finalize();

        WherePredicate pred; pred.has_where=true; pred.col="dict_cat"; pred.op="="; pred.val="Electronics";
        auto bm3 = PredicateEvaluator::evaluate(pred, D, 4);
        CHECK(PredicateEvaluator::countTrue(bm3)==2,
              "dict pred: DICTIONARY column predicate finds 2 matches");
        CHECK(bm3[0]&&!bm3[1]&&bm3[2]&&!bm3[3],
              "dict pred: rows 0,2 pass for DICTIONARY column");
    }

    // RLE column predicate -- must work through reader abstraction
    {
        std::vector<int64_t> rle_vals;
        for (int v=1; v<=5; v++) for (int r=0; r<1000; r++) rle_vals.push_back(v*100LL);

        ColumnWriter w(D,"rle_val",ColumnType::INT64,Encoding::RLE);
        w.setRowCount(rle_vals.size()); w.writeRLE_Int64(rle_vals); w.finalize();

        WherePredicate pred; pred.has_where=true; pred.col="rle_val"; pred.op="="; pred.val="300";
        auto bm4 = PredicateEvaluator::evaluate(pred, D, rle_vals.size());
        CHECK(PredicateEvaluator::countTrue(bm4)==1000,
              "RLE pred: exactly 1000 rows match value 300");
    }

    // evaluateWithReader resets the reader after use
    {
        ColumnReader r; r.open(D,"val");
        WherePredicate pred; pred.has_where=true; pred.col="val"; pred.op=">"; pred.val="25";
        PredicateEvaluator::evaluateWithReader(pred, r);
        CHECK(r.hasNext(), "evaluateWithReader: reader reset after evaluation");
        r.close();
    }

    // countTrue edge cases
    CHECK(PredicateEvaluator::countTrue(Bitmap(10,true))  == 10, "countTrue: all-true");
    CHECK(PredicateEvaluator::countTrue(Bitmap(10,false)) == 0,  "countTrue: all-false");
    CHECK(PredicateEvaluator::countTrue({})               == 0,  "countTrue: empty bitmap");

    rmDir(D);
}

// ============================================================
// SECTION 13 -- End-to-end Phase 1 (NONE encoding)
// ============================================================
static void test_e2e_phase1() {
    SECTION("13. End-to-end Phase 1 (NONE encoding queries)");
    const std::string D = BASE + "/e2e_p1";
    rmDir(D); mkdirP(D);

    // Build small table: id(INT64,NONE), price(DOUBLE,NONE), cat(STRING,NONE)
    std::vector<int64_t>    ids    = {1,2,3,4,5};
    std::vector<double>     prices = {9.99,24.99,4.99,49.99,14.99};
    std::vector<std::string> cats  = {"Books","Electronics","Books","Electronics","Clothing"};

    {
        ColumnWriter wi(D,"id",   ColumnType::INT64,  Encoding::NONE);
        wi.setRowCount(5); wi.writeInt64(ids); wi.finalize();
        ColumnWriter wp(D,"price",ColumnType::DOUBLE, Encoding::NONE);
        wp.setRowCount(5); wp.writeDouble(prices); wp.finalize();
        ColumnWriter wc(D,"cat",  ColumnType::STRING, Encoding::NONE);
        wc.setRowCount(5); wc.writeString(cats); wc.finalize();
        SchemaManager::writeSchema(D, {
            {"id",    ColumnType::INT64,  Encoding::NONE},
            {"price", ColumnType::DOUBLE, Encoding::NONE},
            {"cat",   ColumnType::STRING, Encoding::NONE},
        }, "p1");
    }

    auto schema = SchemaManager::readSchema(D);
    CHECK(schema.size() == 3, "e2e p1: schema has 3 columns");

    // Manual SUM(price) WHERE price > 10 = 24.99+49.99+14.99 = 89.97
    {
        ColumnReader rp; rp.open(D,"price");
        WherePredicate pred; pred.has_where=true; pred.col="price"; pred.op=">"; pred.val="10";
        auto bm = PredicateEvaluator::evaluateWithReader(pred, rp);
        CHECK(PredicateEvaluator::countTrue(bm)==3, "e2e p1: 3 rows pass price>10");

        double sum=0; size_t row=0;
        while (rp.hasNext()) {
            auto v = rp.next();
            if (bm[row]) sum += std::get<double>(v);
            row++;
        }
        CHECK(std::abs(sum - 89.97) < 0.001, "e2e p1: SUM(price) WHERE price>10 = 89.97");
        rp.close();
    }

    // Manual COUNT(*) WHERE cat = 'Books' = 2
    {
        ColumnReader rc; rc.open(D,"cat");
        WherePredicate pred; pred.has_where=true; pred.col="cat"; pred.op="="; pred.val="Books";
        auto bm = PredicateEvaluator::evaluateWithReader(pred, rc);
        CHECK(PredicateEvaluator::countTrue(bm)==2, "e2e p1: COUNT(*) WHERE cat=Books = 2");
        rc.close();
    }

    // Executor: SELECT SUM(price) FROM p1 (no WHERE)
    {
        QueryPlan plan;
        QueryParser("SELECT SUM(price) FROM p1").parse(plan);
        std::deque<ColumnReader> store(1);
        std::vector<ColumnReader*> readers;
        store[0].open(D,"price"); readers.push_back(&store[0]);
        Bitmap bm(5, true);
        std::ostringstream out;
        auto* oldbuf = std::cout.rdbuf(out.rdbuf());
        bool ok = Executor::run(plan, bm, readers, {"price"}, schema, D);
        std::cout.rdbuf(oldbuf);
        CHECK(ok, "e2e p1 executor: SUM(price) runs without error");
        CHECK(out.str().find("104") != std::string::npos,
              "e2e p1 executor: SUM(price) output contains 104 (9.99+24.99+4.99+49.99+14.99)");
        store[0].close();
    }

    rmDir(D);
}

// ============================================================
// SECTION 14 -- End-to-end Phase 2 (DICT + RLE queries)
// ============================================================
static void test_e2e_phase2() {
    SECTION("14. End-to-end Phase 2 (DICT + RLE encoding queries)");
    const std::string D = BASE + "/e2e_p2";
    rmDir(D); mkdirP(D);

    // Build table with dictionary country column and RLE date column
    std::vector<std::string> countries;
    std::vector<int64_t>     dates;
    std::vector<double>      prices;

    // 9000 rows: 3 countries, dates sorted (1000 rows per date value)
    const char* ctries[] = {"US","UK","DE"};
    for (int i=0; i<9000; i++) {
        countries.push_back(ctries[i%3]);
        dates.push_back(20240101LL + (i/1000));  // 9 distinct dates, 1000 each
        prices.push_back((i%3 == 0) ? 10.0 : (i%3==1) ? 20.0 : 30.0);
    }

    {
        ColumnWriter wc(D,"country",ColumnType::STRING,Encoding::DICTIONARY);
        wc.setRowCount(9000); wc.writeDictionary(countries); wc.finalize();

        ColumnWriter wd(D,"date",ColumnType::INT64,Encoding::RLE);
        wd.setRowCount(9000); wd.writeRLE_Int64(dates); wd.finalize();

        ColumnWriter wp(D,"price",ColumnType::DOUBLE,Encoding::NONE);
        wp.setRowCount(9000); wp.writeDouble(prices); wp.finalize();

        SchemaManager::writeSchema(D, {
            {"country", ColumnType::STRING, Encoding::DICTIONARY},
            {"date",    ColumnType::INT64,  Encoding::RLE},
            {"price",   ColumnType::DOUBLE, Encoding::NONE},
        }, "p2");
    }

    // DICT column predicate: WHERE country = 'US'  -> 3000 rows
    {
        ColumnReader rc; rc.open(D,"country");
        WherePredicate pred; pred.has_where=true; pred.col="country"; pred.op="="; pred.val="US";
        auto bm = PredicateEvaluator::evaluateWithReader(pred, rc);
        CHECK(PredicateEvaluator::countTrue(bm)==3000,
              "e2e p2: DICT predicate WHERE country=US finds 3000 rows");
        rc.close();
    }

    // RLE column predicate: WHERE date = 20240101 -> 1000 rows
    {
        ColumnReader rd; rd.open(D,"date");
        WherePredicate pred; pred.has_where=true; pred.col="date"; pred.op="="; pred.val="20240101";
        auto bm = PredicateEvaluator::evaluateWithReader(pred, rd);
        CHECK(PredicateEvaluator::countTrue(bm)==1000,
              "e2e p2: RLE predicate WHERE date=20240101 finds 1000 rows");
        rd.close();
    }

    // Cross-encoding: bitmap from DICT, apply to NONE price column
    {
        ColumnReader rc; rc.open(D,"country");
        WherePredicate pred; pred.has_where=true; pred.col="country"; pred.op="="; pred.val="DE";
        auto bm = PredicateEvaluator::evaluateWithReader(pred, rc);
        rc.close();

        ColumnReader rp; rp.open(D,"price");
        double sum=0; size_t row=0;
        while (rp.hasNext()) { auto v=rp.next(); if(bm[row]) sum+=std::get<double>(v); row++; }
        rp.close();
        // DE rows: i%3==2, price=30.0, 3000 rows -> sum = 90000
        CHECK(std::abs(sum - 90000.0) < 0.001,
              "e2e p2: SUM(price) WHERE country=DE = 90000 (DICT bitmap + NONE column)");
    }

    rmDir(D);
}

// ============================================================
// SECTION 15 -- GROUP BY with DICTIONARY columns (Phase 2)
// ============================================================
static void test_groupby_dict() {
    SECTION("15. GROUP BY with DICTIONARY columns (Phase 2 executor)");
    const std::string D = BASE + "/gb_dict";
    rmDir(D); mkdirP(D);

    // Table: country(DICT), price(NONE)
    // US: rows 0,2,5 -> prices 10,30,60 -> SUM=100
    // UK: rows 1,4   -> prices 20,50    -> SUM=70
    // DE: row 3      -> price 40        -> SUM=40
    std::vector<std::string> countries = {"US","UK","US","DE","UK","US"};
    std::vector<double>      prices    = {10,20,30,40,50,60};

    {
        ColumnWriter wc(D,"country",ColumnType::STRING,Encoding::DICTIONARY);
        wc.setRowCount(6); wc.writeDictionary(countries); wc.finalize();
        ColumnWriter wp(D,"price",ColumnType::DOUBLE,Encoding::NONE);
        wp.setRowCount(6); wp.writeDouble(prices); wp.finalize();
        SchemaManager::writeSchema(D, {
            {"country",ColumnType::STRING,Encoding::DICTIONARY},
            {"price",  ColumnType::DOUBLE,Encoding::NONE}
        }, "gb");
    }

    auto schema = SchemaManager::readSchema(D);
    QueryPlan plan;
    QueryParser("SELECT country, SUM(price) FROM gb GROUP BY country").parse(plan);

    std::deque<ColumnReader> store(2);
    std::vector<ColumnReader*> readers;
    store[0].open(D,"country"); readers.push_back(&store[0]);
    store[1].open(D,"price");   readers.push_back(&store[1]);
    Bitmap bm(6, true);

    std::ostringstream out;
    auto* oldbuf = std::cout.rdbuf(out.rdbuf());
    bool ok = Executor::run(plan, bm, readers, {"country","price"}, schema, D);
    std::cout.rdbuf(oldbuf);

    CHECK(ok, "GROUP BY DICT: executor runs without error");
    std::string output = out.str();
    CHECK(output.find("US") != std::string::npos,  "GROUP BY DICT: US in output");
    CHECK(output.find("UK") != std::string::npos,  "GROUP BY DICT: UK in output");
    CHECK(output.find("DE") != std::string::npos,  "GROUP BY DICT: DE in output");
    CHECK(output.find("100") != std::string::npos, "GROUP BY DICT: SUM(US)=100 in output");
    CHECK(output.find("70")  != std::string::npos, "GROUP BY DICT: SUM(UK)=70 in output");
    CHECK(output.find("40")  != std::string::npos, "GROUP BY DICT: SUM(DE)=40 in output");
    CHECK(output.find("3 groups") != std::string::npos, "GROUP BY DICT: 3 groups in output");

    for (auto& r : store) r.close();

    // GROUP BY with WHERE filter: WHERE price > 25
    {
        std::deque<ColumnReader> st2(2);
        std::vector<ColumnReader*> rd2;
        st2[0].open(D,"country"); rd2.push_back(&st2[0]);
        st2[1].open(D,"price");   rd2.push_back(&st2[1]);

        WherePredicate pred; pred.has_where=true; pred.col="price"; pred.op=">"; pred.val="25";
        auto bm2 = PredicateEvaluator::evaluateWithReader(pred, *rd2[1]);

        QueryPlan plan2;
        QueryParser("SELECT country, SUM(price) FROM gb GROUP BY country").parse(plan2);

        std::ostringstream out2;
        auto* ob2 = std::cout.rdbuf(out2.rdbuf());
        Executor::run(plan2, bm2, rd2, {"country","price"}, schema, D);
        std::cout.rdbuf(ob2);

        std::string o2 = out2.str();
        // price>25: rows 2(30),3(40),4(50),5(60) -> US:30+60=90, UK:50, DE:40
        CHECK(o2.find("90") != std::string::npos,
              "GROUP BY with WHERE: US SUM = 90 (price>25)");

        for (auto& r : st2) r.close();
    }

    rmDir(D);
}

// ============================================================
// SECTION 16 -- File format spec compliance (Section 4)
// ============================================================
static void test_file_format() {
    SECTION("16. File format spec compliance (Manual Section 4)");
    const std::string D = BASE + "/fmt";
    rmDir(D); mkdirP(D);

    // Write a known INT64 column and inspect the raw bytes
    std::vector<int64_t> vals = {100LL, 200LL, 300LL};
    {
        ColumnWriter w(D,"known",ColumnType::INT64,Encoding::NONE);
        w.setRowCount(3); w.writeInt64(vals); w.finalize();
    }

    std::ifstream f(D+"/known.col", std::ios::binary);
    std::vector<char> raw((std::istreambuf_iterator<char>(f)),
                           std::istreambuf_iterator<char>());
    f.close();

    // File size: 36 + 24 + 8 = 68 bytes
    CHECK(raw.size() == 68, "format: INT64(3 rows) file is exactly 68 bytes");

    // magic at offset 0-3: read as uint32_t and compare to MAGIC constant.
    // NOTE: On a little-endian machine the bytes on disk are stored in
    // reversed order (0x31,0x4C,0x4F,0x43), so we must NOT compare individual
    // chars directly. Instead read the uint32_t and compare to MAGIC (0x434F4C31).
    uint32_t magic_val; memcpy(&magic_val, raw.data(), 4);
    CHECK(magic_val == MAGIC,
          "format: magic bytes == MAGIC constant (0x434F4C31) at offset 0");

    // version at offset 4-7 == 1
    uint32_t ver; memcpy(&ver, raw.data()+4, 4);
    CHECK(ver == 1, "format: version == 1 at offset 4");

    // type at offset 8 == INT64 = 2
    CHECK(static_cast<uint8_t>(raw[8]) == 2,
          "format: type byte == 2 (INT64) at offset 8");

    // encoding at offset 9 == NONE = 0
    CHECK(static_cast<uint8_t>(raw[9]) == 0,
          "format: encoding byte == 0 (NONE) at offset 9");

    // reserved at offset 10-11 == 0
    uint16_t res; memcpy(&res, raw.data()+10, 2);
    CHECK(res == 0, "format: reserved field == 0 at offset 10");

    // row_count at offset 12-19 == 3
    uint64_t rc; memcpy(&rc, raw.data()+12, 8);
    CHECK(rc == 3, "format: row_count == 3 at offset 12");

    // data_size at offset 20-27 == 24 (3 * 8 bytes)
    uint64_t ds; memcpy(&ds, raw.data()+20, 8);
    CHECK(ds == 24, "format: data_size == 24 at offset 20");

    // dict_size at offset 28-35 == 0 (no dictionary)
    uint64_t dictS; memcpy(&dictS, raw.data()+28, 8);
    CHECK(dictS == 0, "format: dict_size == 0 for NONE encoding at offset 28");

    // Data section starts at offset 36 -- first value == 100
    int64_t first; memcpy(&first, raw.data()+36, 8);
    CHECK(first == 100LL, "format: data section starts at offset 36, first value 100");

    // Last 8 bytes are CRC64 (must be non-zero for non-empty data)
    uint64_t crc_stored; memcpy(&crc_stored, raw.data()+60, 8);
    CHECK(crc_stored != 0, "format: CRC64 footer at end is non-zero");

    // CRC must validate against content
    uint64_t crc_computed = crc64(raw.data(), raw.size()-8);
    CHECK(crc_stored == crc_computed, "format: CRC64 footer validates correctly");

    // DICTIONARY column: dict_size must be > 0
    {
        ColumnWriter w(D,"dict_fmt",ColumnType::STRING,Encoding::DICTIONARY);
        w.setRowCount(3); w.writeDictionary({"A","B","A"}); w.finalize();

        std::ifstream fd(D+"/dict_fmt.col", std::ios::binary);
        std::vector<char> rd((std::istreambuf_iterator<char>(fd)),
                              std::istreambuf_iterator<char>());
        fd.close();

        uint8_t enc_byte = static_cast<uint8_t>(rd[9]);
        CHECK(enc_byte == 1, "format DICT: encoding byte == 1 (DICTIONARY) at offset 9");

        uint64_t dsize; memcpy(&dsize, rd.data()+28, 8);
        CHECK(dsize > 0, "format DICT: dict_size > 0 in header");
    }

    // RLE column: encoding byte == 2
    {
        std::vector<int64_t> rle_vals(1000, 42LL);
        ColumnWriter w(D,"rle_fmt",ColumnType::INT64,Encoding::RLE);
        w.setRowCount(1000); w.writeRLE_Int64(rle_vals); w.finalize();

        std::ifstream fr(D+"/rle_fmt.col", std::ios::binary);
        std::vector<char> rr((std::istreambuf_iterator<char>(fr)),
                              std::istreambuf_iterator<char>());
        fr.close();

        CHECK(static_cast<uint8_t>(rr[9]) == 2,
              "format RLE: encoding byte == 2 (RLE) at offset 9");
    }

    rmDir(D);
}

// ============================================================
// SECTION 17 -- Loader integration (Section 5 full pipeline)
// ============================================================
static void test_loader_integration() {
    SECTION("17. Loader integration (Section 5 full pipeline)");
    const std::string WH = BASE + "/loader_wh";
    rmDir(WH); mkdirP(WH);  // pre-create warehouse dir so loader can mkdir one more level

    // Write a small CSV that exercises all types
    std::string csv = BASE + "/loader_test.csv";
    {
        std::ofstream f(csv);
        f << "id,price,country,quantity\n";
        // 400 rows: id sequential, price float, country 4 distinct, quantity sorted
        const char* ctries[] = {"US","UK","DE","FR"};
        for (int i=0; i<400; i++) {
            int qty = (i/100) + 1;  // 1,1,...,2,2,...,3,3,...,4,4... (100 per value)
            f << i << "," << (i*0.99+1.0) << "," << ctries[i%4] << "," << qty << "\n";
        }
    }

    // Load should succeed
    CHECK(Loader::load(csv, "sales", WH), "loader: load returns true");

    std::string tdir = WH + "/sales";

    // schema.json must exist
    CHECK(SchemaManager::schemaExists(tdir), "loader: schema.json written");

    // All 4 .col files must exist
    CHECK(fileSize(tdir+"/id.col")       > 0, "loader: id.col exists");
    CHECK(fileSize(tdir+"/price.col")    > 0, "loader: price.col exists");
    CHECK(fileSize(tdir+"/country.col")  > 0, "loader: country.col exists");
    CHECK(fileSize(tdir+"/quantity.col") > 0, "loader: quantity.col exists");

    // No .tmp files left behind
    CHECK(fileSize(tdir+"/id.col.tmp")       == -1, "loader: id.col.tmp removed");
    CHECK(fileSize(tdir+"/schema.json.tmp")  == -1, "loader: schema.json.tmp removed");

    // Read schema
    auto schema = SchemaManager::readSchema(tdir);
    CHECK(schema.size() == 4, "loader: schema has 4 columns");

    // id is INT64 (sequential, no runs) -> NONE
    // price is DOUBLE (random) -> NONE
    // country is STRING (4 distinct) -> DICTIONARY
    // quantity is INT64 (sorted in runs) -> RLE
    std::string id_enc="", price_enc="", country_enc="", qty_enc="";
    for (auto& sc : schema) {
        if (sc.name=="id")       id_enc      = encodingToString(sc.encoding);
        if (sc.name=="price")    price_enc   = encodingToString(sc.encoding);
        if (sc.name=="country")  country_enc = encodingToString(sc.encoding);
        if (sc.name=="quantity") qty_enc     = encodingToString(sc.encoding);
    }
    // BUG FIX (Bug 3): type_inference now returns INT32 for small integers.
    // id (0-399) and quantity (1-4) both fit in INT32 range.
    CHECK(id_enc      == "none",       "loader: id -> NONE (sequential INT32, no runs)");
    CHECK(price_enc   == "none",       "loader: price -> NONE (random double)");
    CHECK(country_enc == "dictionary", "loader: country -> DICTIONARY (4 distinct)");
    CHECK(qty_enc     == "rle",        "loader: quantity -> RLE (INT32, 100x repeated values)");

    // All 4 columns are readable after load
    // Verify type assignments match updated type_inference (INT32 for small ints)
    for (const auto& sc : schema) {
        if (sc.name == "id" || sc.name == "quantity")
            CHECK(sc.type == ColumnType::INT32, "loader: " + sc.name + " type is INT32");
        if (sc.name == "price")
            CHECK(sc.type == ColumnType::DOUBLE, "loader: price type is DOUBLE");
        if (sc.name == "country")
            CHECK(sc.type == ColumnType::STRING, "loader: country type is STRING");
    }

    for (const auto& col : schema) {
        ColumnReader r;
        CHECK(r.open(tdir, col.name),           "loader: " + col.name + " column opens");
        CHECK(r.getRowCount() == 400,           "loader: " + col.name + " has 400 rows");
        r.close();
    }

    // country column decodes all 400 values correctly through DICTIONARY
    {
        ColumnReader r; r.open(tdir,"country");
        auto vals = readAll(r);
        r.close();
        const char* ctries[] = {"US","UK","DE","FR"};
        bool ok = true;
        for (size_t i=0; i<400; i++)
            if (std::get<std::string>(vals[i]) != ctries[i%4]) { ok=false; break; }
        CHECK(ok, "loader: country DICTIONARY decodes all 400 values correctly");
    }

    // quantity column decodes all 400 values correctly through RLE
    {
        ColumnReader r; r.open(tdir,"quantity");
        auto vals = readAll(r);
        r.close();
        bool ok = true;
        for (size_t i=0; i<400; i++) {
            int64_t expected = (i/100) + 1;
            if (std::get<int64_t>(vals[i]) != expected) { ok=false; break; }
        }
        CHECK(ok, "loader: quantity RLE decodes all 400 values correctly");
    }

    // Reload same table (overwrites) -- should not crash or leave corrupt files
    CHECK(Loader::load(csv, "sales", WH), "loader: re-load same table succeeds");
    CHECK(SchemaManager::schemaExists(tdir), "loader: schema.json still valid after re-load");

    rmDir(WH);
    std::remove(csv.c_str());
}

// ============================================================
// main
// ============================================================
int main() {
    std::error_code ec;
    // Pre-create ALL subdirectories that tests will use.
    // On Windows, system() and filesystem may be blocked for untrusted
    // executables. Creating directories here at startup (before any
    // ColumnWriter runs) ensures they exist when needed.
    // We use a shell command which runs in the trusted cmd.exe context.
    auto pre_mkdir = [](const std::string& p) {
#ifdef _WIN32
        std::string native = p;
        for (char& c : native) if (c == '/') c = '\\';
        std::string cmd = "cmd /c mkdir \"" + native + "\" >nul 2>&1";
        system(cmd.c_str());
#else
        std::error_code ec2;
        std::filesystem::create_directories(p, ec2);
#endif
    };
    pre_mkdir(BASE);
    pre_mkdir(BASE + "/csv");
    pre_mkdir(BASE + "/none");
    pre_mkdir(BASE + "/dict");
    pre_mkdir(BASE + "/rle");
    pre_mkdir(BASE + "/pred");
    pre_mkdir(BASE + "/schema");
    pre_mkdir(BASE + "/e2e_p1");
    pre_mkdir(BASE + "/e2e_p2");
    pre_mkdir(BASE + "/gb_dict");
    pre_mkdir(BASE + "/fmt");
    pre_mkdir(BASE + "/enc_sel_wh");
    pre_mkdir(BASE + "/loader_wh");

    // DIAGNOSTIC: test if exe can write ANY file at all
    {
        FILE* fp = fopen("colsh_test_tmp\\__writetest__.txt", "wb");
        if (fp) {
            fwrite("ok", 1, 2, fp);
            fclose(fp);
            std::cerr << "DIAG: direct fopen in colsh_test_tmp SUCCEEDED\n";
        } else {
            std::cerr << "DIAG: direct fopen in colsh_test_tmp FAILED errno=" << errno << "\n";
        }
        FILE* fp2 = fopen("__writetest2__.txt", "wb");
        if (fp2) {
            fwrite("ok", 1, 2, fp2);
            fclose(fp2);
            std::cerr << "DIAG: direct fopen in CWD SUCCEEDED\n";
        } else {
            std::cerr << "DIAG: direct fopen in CWD FAILED errno=" << errno << "\n";
        }
    }
    std::cout << "============================================\n";
    std::cout << "  Phase 1 + Phase 2 Full Test Suite\n";
    std::cout << "============================================\n";

    test_crc64();
    test_converters();
    test_header_layout();
    test_type_inference();
    test_csv_parser();
    test_writer_reader_none();
    test_dictionary();
    test_rle();
    test_encoding_selection();
    test_schema_manager();
    test_query_parser();
    test_predicate();
    test_e2e_phase1();
    test_e2e_phase2();
    test_groupby_dict();
    test_file_format();
    test_loader_integration();

    // Print final section summary
    if (g_section_passed + g_section_failed > 0) {
        std::cout << "         (" << g_section_passed << " passed";
        if (g_section_failed) std::cout << ", " << g_section_failed << " FAILED";
        std::cout << ")\n";
    }

    std::cout << "\n============================================\n";
    std::cout << "  Results: " << g_passed << " passed, "
              << g_failed << " failed\n";
    std::cout << "============================================\n";

    // Clean up base temp dir
    std::filesystem::remove_all(BASE, ec);

    return g_failed == 0 ? 0 : 1;

}