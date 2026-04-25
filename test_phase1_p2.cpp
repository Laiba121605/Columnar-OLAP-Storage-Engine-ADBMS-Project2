// test_phase1_p2.cpp — P2 Complete Test Suite
// Column Reader + Query Parser + Predicate Evaluator
//
// Compile (Linux/WSL):
//   g++ -std=c++17 -I include test_phase1_p2.cpp src/*.cpp -o test_p2
//
// Compile (Windows Developer Command Prompt):
//   cl /std:c++17 /I include /EHsc test_phase1_p2.cpp src\column_writer.cpp src\common.cpp src\csv_parser.cpp src\loader.cpp src\schema.cpp src\type_inference.cpp src\column_reader.cpp src\query_parser.cpp src\predicate.cpp /Fe:test_p2.exe

#include <iostream>
#include <cassert>
#include <fstream>
#include <string>
#include <variant>
#include <cstdio>
#include "include/common.h"
#include "include/column_writer.h"
#include "include/schema.h"
#include "include/column_reader.h"
#include "include/query_parser.h"
#include "include/predicate.h"

using namespace std;

// ── helpers ─────────────────────────────────────────────────
void createDirectory(const string& path) {
#ifdef _WIN32
    string cmd = "if not exist \"" + path + "\" mkdir \"" + path + "\"";
#else
    string cmd = "mkdir -p \"" + path + "\"";
#endif
    system(cmd.c_str());
}

// Windows-safe file delete before writing
// std::rename fails on Windows if destination already exists.
// Deleting first ensures rename always succeeds.
void deleteFile(const string& path) {
    std::remove(path.c_str());
}

void cleanDir(const string& dir) {
    deleteFile(dir + "/id.col");
    deleteFile(dir + "/id.col.tmp");
    deleteFile(dir + "/price.col");
    deleteFile(dir + "/price.col.tmp");
    deleteFile(dir + "/country.col");
    deleteFile(dir + "/country.col.tmp");
    deleteFile(dir + "/quantity.col");
    deleteFile(dir + "/quantity.col.tmp");
    deleteFile(dir + "/schema.json");
    deleteFile(dir + "/schema.json.tmp");
}

// Sets up a fresh 6-row test table — cleans old files first
void setupTestColumns(const string& dir) {
    createDirectory(dir);
    cleanDir(dir);  // delete old files so Windows rename works

    { ColumnWriter w(dir,"id",ColumnType::INT64,Encoding::NONE); w.setRowCount(6); w.writeInt64({1,2,3,4,5,6}); w.finalize(); }
    { ColumnWriter w(dir,"price",ColumnType::DOUBLE,Encoding::NONE); w.setRowCount(6); w.writeDouble({10.0,20.0,30.0,40.0,50.0,60.0}); w.finalize(); }
    { ColumnWriter w(dir,"country",ColumnType::STRING,Encoding::NONE); w.setRowCount(6); w.writeString({"Pakistan","USA","UK","Germany","Pakistan","USA"}); w.finalize(); }
    { ColumnWriter w(dir,"quantity",ColumnType::INT64,Encoding::NONE); w.setRowCount(6); w.writeInt64({1,2,3,4,5,6}); w.finalize(); }

    vector<SchemaColumn> cols = {
        {"id",       ColumnType::INT64,  Encoding::NONE},
        {"price",    ColumnType::DOUBLE, Encoding::NONE},
        {"country",  ColumnType::STRING, Encoding::NONE},
        {"quantity", ColumnType::INT64,  Encoding::NONE}
    };
    SchemaManager::writeSchema(dir, cols, "test_table");
}

// ════════════════════════════════════════════════════════════
// COLUMN READER TESTS (1-7)
// ════════════════════════════════════════════════════════════

void testReadInt64() {
    cout << "\n[P2 TEST 1] Read INT64 column\n";
    cout << "----------------------------------------\n";
    string dir = "data/p2_test";
    setupTestColumns(dir);
    ColumnReader r;
    assert(r.open(dir, "id"));
    assert(r.getType()     == ColumnType::INT64);
    assert(r.getEncoding() == Encoding::NONE);
    assert(r.getRowCount() == 6);
    vector<int64_t> vals;
    while (r.hasNext()) vals.push_back(get<int64_t>(r.next()));
    r.close();
    assert(vals.size()==6 && vals[0]==1 && vals[5]==6);
    cout << "  Values: "; for (auto v:vals) cout<<v<<" "; cout<<"\n";
    cout << "  ✓ INT64 read PASSED\n";
}

void testReadDouble() {
    cout << "\n[P2 TEST 2] Read DOUBLE column\n";
    cout << "----------------------------------------\n";
    ColumnReader r;
    assert(r.open("data/p2_test","price"));
    vector<double> vals;
    while (r.hasNext()) vals.push_back(get<double>(r.next()));
    r.close();
    assert(vals.size()==6 && vals[0]==10.0 && vals[5]==60.0);
    cout << "  Values: "; for (auto v:vals) cout<<v<<" "; cout<<"\n";
    cout << "  ✓ DOUBLE read PASSED\n";
}

void testReadString() {
    cout << "\n[P2 TEST 3] Read STRING column\n";
    cout << "----------------------------------------\n";
    ColumnReader r;
    assert(r.open("data/p2_test","country"));
    vector<string> vals;
    while (r.hasNext()) vals.push_back(get<string>(r.next()));
    r.close();
    assert(vals.size()==6 && vals[0]=="Pakistan" && vals[2]=="UK");
    cout << "  Values: "; for (auto& v:vals) cout<<v<<" "; cout<<"\n";
    cout << "  ✓ STRING read PASSED\n";
}

void testReset() {
    cout << "\n[P2 TEST 4] reset() — scan column twice\n";
    cout << "----------------------------------------\n";
    ColumnReader r;
    assert(r.open("data/p2_test","id"));
    vector<int64_t> first, second;
    while (r.hasNext()) first.push_back(get<int64_t>(r.next()));
    r.reset();
    while (r.hasNext()) second.push_back(get<int64_t>(r.next()));
    r.close();
    assert(first == second);
    cout << "  First:  "; for (auto v:first)  cout<<v<<" "; cout<<"\n";
    cout << "  Second: "; for (auto v:second) cout<<v<<" "; cout<<"\n";
    cout << "  ✓ reset() PASSED\n";
}

void testCRCDetection() {
    cout << "\n[P2 TEST 5] CRC corruption detected on open\n";
    cout << "----------------------------------------\n";

    // Corrupt id.col
    {
        fstream f("data/p2_test/id.col", ios::binary|ios::in|ios::out);
        f.seekp(40, ios::beg);
        char bad = 0xFF;
        f.write(&bad, 1);
    }

    // open() should fail
    ColumnReader r;
    assert(!r.open("data/p2_test","id"));
    cout << "  Error: " << r.getLastError() << "\n";
    cout << "  ✓ CRC detection PASSED\n";

    // Restore clean files for remaining tests
    setupTestColumns("data/p2_test");
    cout << "  Files restored\n";
}

void testMissingFile() {
    cout << "\n[P2 TEST 6] Missing file handled gracefully\n";
    cout << "----------------------------------------\n";
    ColumnReader r;
    assert(!r.open("data/p2_test","nonexistent"));
    assert(!r.isOpen());
    cout << "  Error: " << r.getLastError() << "\n";
    cout << "  ✓ Missing file PASSED\n";
}

void testSchemaDriver() {
    cout << "\n[P2 TEST 7] Schema-driven multi-column open\n";
    cout << "----------------------------------------\n";
    string dir = "data/p2_test";
    auto schema = SchemaManager::readSchema(dir);
    assert(schema.size() == 4);
    ColumnReader r_price, r_country;
    assert(r_price.open(dir,"price"));
    assert(r_country.open(dir,"country"));
    cout << "  Opened 2 of 4 columns (columnar advantage)\n";
    cout << "  Row data:\n";
    while (r_price.hasNext()) {
        double price   = get<double>(r_price.next());
        string country = get<string>(r_country.next());
        cout << "    price=" << price << "  country=" << country << "\n";
    }
    r_price.close(); r_country.close();
    cout << "  ✓ Schema-driven open PASSED\n";
}

// ════════════════════════════════════════════════════════════
// QUERY PARSER TESTS (8-14)
// ════════════════════════════════════════════════════════════

void testParseSimpleSelect() {
    cout << "\n[P2 TEST 8] Parse simple SELECT\n";
    cout << "----------------------------------------\n";
    QueryParser p("SELECT id, price FROM sales");
    QueryPlan plan;
    assert(p.parse(plan));
    assert(plan.table == "sales");
    assert(plan.selects.size() == 2);
    assert(plan.selects[0].col == "id");
    assert(plan.selects[1].col == "price");
    assert(!plan.where.has_where);
    printQueryPlan(plan);
    cout << "  ✓ Simple SELECT PASSED\n";
}

void testParseSUM() {
    cout << "\n[P2 TEST 9] Parse SUM with WHERE\n";
    cout << "----------------------------------------\n";
    QueryParser p("SELECT SUM(price) FROM sales WHERE date >= 20240101");
    QueryPlan plan;
    assert(p.parse(plan));
    assert(plan.selects[0].agg == AggFunc::SUM);
    assert(plan.selects[0].col == "price");
    assert(plan.where.has_where);
    assert(plan.where.op == ">=");
    assert(plan.where.val == "20240101");
    auto needed = plan.neededColumns();
    assert(needed.size() == 2);
    printQueryPlan(plan);
    cout << "  ✓ SUM with WHERE PASSED\n";
}

void testParseCountStar() {
    cout << "\n[P2 TEST 10] Parse COUNT(*)\n";
    cout << "----------------------------------------\n";
    QueryParser p("SELECT COUNT(*) FROM sales WHERE category = 'Electronics'");
    QueryPlan plan;
    assert(p.parse(plan));
    assert(plan.selects[0].agg  == AggFunc::COUNT);
    assert(plan.selects[0].star == true);
    assert(plan.where.val == "Electronics");
    printQueryPlan(plan);
    cout << "  ✓ COUNT(*) PASSED\n";
}

void testParseSelectStar() {
    cout << "\n[P2 TEST 11] Parse SELECT *\n";
    cout << "----------------------------------------\n";
    QueryParser p("SELECT * FROM sales WHERE id = 500000");
    QueryPlan plan;
    assert(p.parse(plan));
    assert(plan.select_star);
    assert(plan.where.col == "id");
    printQueryPlan(plan);
    cout << "  ✓ SELECT * PASSED\n";
}

void testParseGroupBy() {
    cout << "\n[P2 TEST 12] Parse GROUP BY\n";
    cout << "----------------------------------------\n";
    QueryParser p("SELECT country, AVG(quantity) FROM sales GROUP BY country");
    QueryPlan plan;
    assert(p.parse(plan));
    assert(plan.selects[1].agg == AggFunc::AVG);
    assert(plan.groupby == "country");
    printQueryPlan(plan);
    cout << "  ✓ GROUP BY PASSED\n";
}

void testParseError() {
    cout << "\n[P2 TEST 13] Parse error handling\n";
    cout << "----------------------------------------\n";
    QueryParser p2("SELECT price sales");
    QueryPlan plan2;
    assert(!p2.parse(plan2));
    cout << "  'SELECT price sales': " << p2.getError() << "\n";
    QueryParser p3("");
    QueryPlan plan3;
    assert(!p3.parse(plan3));
    cout << "  Empty query: " << p3.getError() << "\n";
    cout << "  ✓ Error handling PASSED\n";
}

void testNeededColumns() {
    cout << "\n[P2 TEST 14] neededColumns() — columnar I/O\n";
    cout << "----------------------------------------\n";
    QueryParser p("SELECT country, SUM(price) FROM sales WHERE date >= 20240101 GROUP BY country");
    QueryPlan plan;
    assert(p.parse(plan));
    auto cols = plan.neededColumns();
    cout << "  Needs: "; for (auto& c:cols) cout<<c<<" "; cout<<"\n";
    assert(cols.size() == 3);
    cout << "  Opens 3 of 7 columns — columnar advantage!\n";
    cout << "  ✓ neededColumns() PASSED\n";
}

// ════════════════════════════════════════════════════════════
// PREDICATE EVALUATOR TESTS (15-20)
// ════════════════════════════════════════════════════════════

void testPredicateNoWhere() {
    cout << "\n[P2 TEST 15] Predicate — no WHERE (all rows pass)\n";
    cout << "----------------------------------------\n";
    WherePredicate pred;
    Bitmap bm = PredicateEvaluator::evaluate(pred, "data/p2_test", 6);
    assert(bm.size() == 6);
    assert(PredicateEvaluator::countTrue(bm) == 6);
    PredicateEvaluator::printBitmap(bm);
    cout << "  ✓ No WHERE PASSED\n";
}

void testPredicateInt64GT() {
    cout << "\n[P2 TEST 16] Predicate — INT64 WHERE id > 3\n";
    cout << "----------------------------------------\n";
    WherePredicate pred;
    pred.has_where = true;
    pred.col = "id";
    pred.op  = ">";
    pred.val = "3";
    Bitmap bm = PredicateEvaluator::evaluate(pred, "data/p2_test", 6);
    assert(bm.size() == 6);
    assert(bm[0]==false && bm[1]==false && bm[2]==false);
    assert(bm[3]==true  && bm[4]==true  && bm[5]==true);
    assert(PredicateEvaluator::countTrue(bm) == 3);
    PredicateEvaluator::printBitmap(bm);
    cout << "  ✓ INT64 > PASSED\n";
}

void testPredicateInt64EQ() {
    cout << "\n[P2 TEST 17] Predicate — INT64 WHERE id = 4\n";
    cout << "----------------------------------------\n";
    WherePredicate pred;
    pred.has_where = true;
    pred.col = "id";
    pred.op  = "=";
    pred.val = "4";
    Bitmap bm = PredicateEvaluator::evaluate(pred, "data/p2_test", 6);
    assert(PredicateEvaluator::countTrue(bm) == 1);
    assert(bm[3] == true);
    PredicateEvaluator::printBitmap(bm);
    cout << "  ✓ INT64 = PASSED\n";
}

void testPredicateDoubleGTE() {
    cout << "\n[P2 TEST 18] Predicate — DOUBLE WHERE price >= 30.0\n";
    cout << "----------------------------------------\n";
    WherePredicate pred;
    pred.has_where = true;
    pred.col = "price";
    pred.op  = ">=";
    pred.val = "30.0";
    Bitmap bm = PredicateEvaluator::evaluate(pred, "data/p2_test", 6);
    assert(bm[0]==false && bm[1]==false);
    assert(bm[2]==true  && bm[3]==true);
    assert(PredicateEvaluator::countTrue(bm) == 4);
    PredicateEvaluator::printBitmap(bm);
    cout << "  ✓ DOUBLE >= PASSED\n";
}

void testPredicateStringEQ() {
    cout << "\n[P2 TEST 19] Predicate — STRING WHERE country = Pakistan\n";
    cout << "----------------------------------------\n";
    WherePredicate pred;
    pred.has_where = true;
    pred.col = "country";
    pred.op  = "=";
    pred.val = "Pakistan";
    Bitmap bm = PredicateEvaluator::evaluate(pred, "data/p2_test", 6);
    assert(bm[0]==true && bm[1]==false && bm[4]==true);
    assert(PredicateEvaluator::countTrue(bm) == 2);
    PredicateEvaluator::printBitmap(bm);
    cout << "  ✓ STRING = PASSED\n";
}

void testPredicateStringNEQ() {
    cout << "\n[P2 TEST 20] Predicate — STRING WHERE country != USA\n";
    cout << "----------------------------------------\n";
    WherePredicate pred;
    pred.has_where = true;
    pred.col = "country";
    pred.op  = "!=";
    pred.val = "USA";
    Bitmap bm = PredicateEvaluator::evaluate(pred, "data/p2_test", 6);
    assert(PredicateEvaluator::countTrue(bm) == 4);
    assert(bm[1]==false && bm[5]==false);
    PredicateEvaluator::printBitmap(bm);
    cout << "  ✓ STRING != PASSED\n";
}

// ════════════════════════════════════════════════════════════
// FULL INTEGRATION TEST (21)
// ════════════════════════════════════════════════════════════
void testFullIntegration() {
    cout << "\n[P2 TEST 21] Full Integration — Parser + Reader + Predicate\n";
    cout << "----------------------------------------\n";
    cout << "  Query: SELECT SUM(price) FROM test_table WHERE id > 3\n";

    string dir = "data/p2_test";

    // Step 1: parse
    QueryParser parser("SELECT SUM(price) FROM test_table WHERE id > 3");
    QueryPlan plan;
    assert(parser.parse(plan));
    cout << "  Parsed: table=" << plan.table << " agg=SUM col=price WHERE id>3\n";

    // Step 2: bitmap
    Bitmap bm = PredicateEvaluator::evaluate(plan.where, dir, 6);
    cout << "  Bitmap: ";
    PredicateEvaluator::printBitmap(bm);
    assert(PredicateEvaluator::countTrue(bm) == 3);

    // Step 3: open price column only
    ColumnReader price_reader;
    assert(price_reader.open(dir, "price"));
    cout << "  Opened: price.col only (1 of 4 columns)\n";

    // Step 4 (P3 simulation): apply bitmap
    double sum = 0.0;
    size_t row = 0;
    while (price_reader.hasNext()) {
        double price = get<double>(price_reader.next());
        if (bm[row]) {
            sum += price;
            cout << "    row " << row << ": price=" << price << " INCLUDED\n";
        } else {
            cout << "    row " << row << ": price=" << price << " skipped\n";
        }
        row++;
    }
    price_reader.close();

    cout << "  SUM(price) WHERE id > 3 = " << sum << "\n";
    assert(sum == 150.0);
    cout << "  ✓ Full integration PASSED (P2->P3 handoff works)\n";
}

// ── main ─────────────────────────────────────────────────────
int main() {
    cout << "\n========================================\n";
    cout << "P2 COMPLETE TEST SUITE\n";
    cout << "Column Reader + Query Parser + Predicate\n";
    cout << "========================================\n";

    testReadInt64();
    testReadDouble();
    testReadString();
    testReset();
    testCRCDetection();
    testMissingFile();
    testSchemaDriver();
    testParseSimpleSelect();
    testParseSUM();
    testParseCountStar();
    testParseSelectStar();
    testParseGroupBy();
    testParseError();
    testNeededColumns();
    testPredicateNoWhere();
    testPredicateInt64GT();
    testPredicateInt64EQ();
    testPredicateDoubleGTE();
    testPredicateStringEQ();
    testPredicateStringNEQ();
    testFullIntegration();

    cout << "\n========================================\n";
    cout << "ALL 21 P2 TESTS PASSED!\n";
    cout << "========================================\n\n";
    cout << "P2 delivers to P3:\n";
    cout << "  QueryPlan     - what table, columns, filter, aggregation\n";
    cout << "  Bitmap        - which rows passed the WHERE clause\n";
    cout << "  ColumnReaders - open files for only needed columns\n";
    return 0;
}
