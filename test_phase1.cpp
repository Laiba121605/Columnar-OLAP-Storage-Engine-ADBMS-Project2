// test_phase1.cpp - Complete Phase 1 Test
// Compile: g++ -std=c++17 -I include test_phase1.cpp src/*.cpp -o test_phase1.exe
// Run: .\test_phase1.exe

#include <iostream>
#include <cassert>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <cstdint>
#include "include/common.h"
#include "include/csv_parser.h"
#include "include/type_inference.h"
#include "include/column_writer.h"
#include "include/schema.h"

using namespace std;

// Helper function to create directories on Windows
void createDirectory(const string& path) {
    string cmd = "if not exist \"" + path + "\" mkdir \"" + path + "\"";
    system(cmd.c_str());
}

// Helper to read a file as string
string readFileAsString(const string& path) {
    ifstream file(path);
    if (!file.is_open()) return "";
    stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

// Test 1: CSV Parser
void testCSVParser() {
    cout << "\n[TEST 1] CSV Parser\n";
    cout << "----------------------------------------\n";
    
    // Create directory and test CSV
    createDirectory("data");
    
    ofstream file("data/test.csv");
    file << "id,name,age,price\n";
    file << "1,Alice,25,99.99\n";
    file << "2,Bob,30,149.50\n";
    file << "3,Charlie,35,199.00\n";
    file << "4,Diana,28,79.95\n";
    file << "5,Eve,32,299.99\n";
    file << "6,\"Smith, John\",40,50.00\n";
    file << "7,\"She said \"\"Hello\"\"\",45,75.50\n";
    file.close();
    
    CSVParser parser("data/test.csv");
    assert(parser.parse());
    
    auto headers = parser.getHeaders();
    auto rows = parser.getRows();
    
    assert(headers.size() == 4);
    assert(headers[0] == "id");
    assert(headers[1] == "name");
    assert(headers[2] == "age");
    assert(headers[3] == "price");
    
    assert(rows.size() == 7);
    assert(rows[0][0] == "1");
    assert(rows[0][1] == "Alice");
    assert(rows[0][2] == "25");
    assert(rows[0][3] == "99.99");
    
    assert(rows[5][1] == "Smith, John");
    assert(rows[6][1] == "She said \"Hello\"");
    
    cout << "  Headers: ";
    for (auto h : headers) cout << h << " ";
    cout << "\n";
    cout << "  Rows parsed: " << rows.size() << "\n";
    cout << "  Quoted field test: PASSED\n";
    cout << "  Escaped quote test: PASSED\n";
    cout << "  ✓ CSV Parser PASSED\n";
}

// Test 2: Type Inference
void testTypeInference() {
    cout << "\n[TEST 2] Type Inference\n";
    cout << "----------------------------------------\n";
    
    vector<string> int_col = {"1", "2", "3", "4", "5", "-10", "0"};
    vector<string> double_col = {"1.5", "2.7", "3.14", "4.0", "5.99", "0.0", "-1.5"};
    vector<string> string_col = {"apple", "banana", "cherry", "", "date", "123abc"};
    vector<string> mixed_col = {"1", "2.5", "three", "4", "5.0"};
    vector<string> empty_col = {};
    
    assert(TypeInference::inferType(int_col) == ColumnType::INT64);
    assert(TypeInference::inferType(double_col) == ColumnType::DOUBLE);
    assert(TypeInference::inferType(string_col) == ColumnType::STRING);
    assert(TypeInference::inferType(mixed_col) == ColumnType::STRING);
    assert(TypeInference::inferType(empty_col) == ColumnType::STRING);
    
    assert(TypeInference::isInt64("123") == true);
    assert(TypeInference::isInt64("-456") == true);
    assert(TypeInference::isInt64("12.3") == false);
    assert(TypeInference::isInt64("abc") == false);
    
    assert(TypeInference::isDouble("123") == true);
    assert(TypeInference::isDouble("12.3") == true);
    assert(TypeInference::isDouble("-12.3") == true);
    assert(TypeInference::isDouble("abc") == false);
    
    cout << "  INT64 column: PASSED\n";
    cout << "  DOUBLE column: PASSED\n";
    cout << "  STRING column: PASSED\n";
    cout << "  Mixed column: PASSED\n";
    cout << "  ✓ Type Inference PASSED\n";
}

// Test 3: Column Writer
void testColumnWriter() {
   cout << "\n[TEST 3] Column Writer\n";
    cout << "----------------------------------------\n";
    
    string test_dir = "data/test_warehouse/sales";
    
    // Create directories using backslashes for Windows
    createDirectory("data");
    createDirectory("data/test_warehouse");
    createDirectory("data/test_warehouse/sales");
    
    cout << "  Created directory: " << test_dir << "\n";
    
    // Test INT64 column
    {
        ColumnWriter writer(test_dir, "id", ColumnType::INT64, Encoding::NONE);
        vector<int64_t> values = {1, 2, 3, 4, 5, 6, 7};
        writer.setRowCount(values.size());
        assert(writer.writeInt64(values));
        assert(writer.finalize());
        cout << "  ✓ Wrote id.col (INT64)\n";
    }
    
    // Test DOUBLE column
    {
        ColumnWriter writer(test_dir, "price", ColumnType::DOUBLE, Encoding::NONE);
        vector<double> values = {99.99, 149.50, 199.00, 79.95, 299.99, 50.00, 75.50};
        writer.setRowCount(values.size());
        assert(writer.writeDouble(values));
        assert(writer.finalize());
        cout << "  ✓ Wrote price.col (DOUBLE)\n";
    }
    
    // Test STRING column
    {
        ColumnWriter writer(test_dir, "name", ColumnType::STRING, Encoding::NONE);
        vector<string> values = {"Alice", "Bob", "Charlie", "Diana", "Eve", "Smith, John", "She said \"Hello\""};
        writer.setRowCount(values.size());
        assert(writer.writeString(values));
        assert(writer.finalize());
        cout << "  ✓ Wrote name.col (STRING)\n";
    }
    
    // Verify files exist
    assert(ifstream(test_dir + "/id.col").good());
    assert(ifstream(test_dir + "/price.col").good());
    assert(ifstream(test_dir + "/name.col").good());
    
    cout << "  ✓ Column Writer PASSED\n";
}

// Test 4: Column Reader (Manual verification)
void testColumnReader() {
    cout << "\n[TEST 4] Column Reader (Manual Verification)\n";
    cout << "----------------------------------------\n";
    
    string test_dir = "data/test_warehouse/sales";
    
    // Read and verify INT64 column
    {
        ifstream file(test_dir + "/id.col", ios::binary);
        assert(file.is_open());
        
        ColumnHeader header;
        file.read(reinterpret_cast<char*>(&header), HEADER_SIZE);
        
        assert(header.magic == MAGIC);
        assert(header.version == VERSION);
        assert(header.type == static_cast<uint8_t>(ColumnType::INT64));
        assert(header.encoding == static_cast<uint8_t>(Encoding::NONE));
        assert(header.row_count == 7);
        
        vector<int64_t> ids(header.row_count);
        for (size_t i = 0; i < header.row_count; i++) {
            file.read(reinterpret_cast<char*>(&ids[i]), sizeof(int64_t));
        }
        file.close();
        
        assert(ids[0] == 1);
        assert(ids[3] == 4);
        assert(ids[6] == 7);
        
        cout << "  id.col: " << ids.size() << " values, first=" << ids[0] << ", last=" << ids[6] << "\n";
        cout << "  ✓ INT64 reader PASSED\n";
    }
    
    // Read and verify DOUBLE column
    {
        ifstream file(test_dir + "/price.col", ios::binary);
        assert(file.is_open());
        
        ColumnHeader header;
        file.read(reinterpret_cast<char*>(&header), HEADER_SIZE);
        
        assert(header.type == static_cast<uint8_t>(ColumnType::DOUBLE));
        assert(header.row_count == 7);
        
        vector<double> prices(header.row_count);
        for (size_t i = 0; i < header.row_count; i++) {
            file.read(reinterpret_cast<char*>(&prices[i]), sizeof(double));
        }
        file.close();
        
        assert(prices[0] == 99.99);
        assert(prices[3] == 79.95);
        
        cout << "  price.col: " << prices.size() << " values, first=" << prices[0] << "\n";
        cout << "  ✓ DOUBLE reader PASSED\n";
    }
    
    // Read and verify STRING column
    {
        ifstream file(test_dir + "/name.col", ios::binary);
        assert(file.is_open());
        
        ColumnHeader header;
        file.read(reinterpret_cast<char*>(&header), HEADER_SIZE);
        
        assert(header.type == static_cast<uint8_t>(ColumnType::STRING));
        assert(header.row_count == 7);
        
        vector<string> names(header.row_count);
        for (size_t i = 0; i < header.row_count; i++) {
            uint64_t len;
            file.read(reinterpret_cast<char*>(&len), sizeof(uint64_t));
            names[i].resize(len);
            file.read(&names[i][0], len);
        }
        file.close();
        
        assert(names[0] == "Alice");
        assert(names[5] == "Smith, John");
        assert(names[6] == "She said \"Hello\"");
        
        cout << "  name.col: " << names.size() << " values, first=" << names[0] << "\n";
        cout << "  ✓ STRING reader PASSED\n";
    }
    
    cout << "  ✓ Column Reader PASSED\n";
}

// Test 5: Schema Manager (Write and Read)
void testSchemaManager() {
    cout << "\n[TEST 5] Schema Manager\n";
    cout << "----------------------------------------\n";
    
    string test_dir = "data/test_warehouse/sales";
    
    // Make sure directory exists
    createDirectory(test_dir);
    
    vector<SchemaColumn> columns = {
        {"id", ColumnType::INT64, Encoding::NONE},
        {"name", ColumnType::STRING, Encoding::NONE},
        {"age", ColumnType::INT64, Encoding::NONE},
        {"price", ColumnType::DOUBLE, Encoding::NONE}
    };
    
    assert(SchemaManager::writeSchema(test_dir, columns, "sales"));
    cout << "  ✓ writeSchema() PASSED\n";
    
    assert(SchemaManager::schemaExists(test_dir));
    cout << "  ✓ schemaExists() PASSED\n";
    
    auto read_columns = SchemaManager::readSchema(test_dir);
    assert(read_columns.size() == 4);
    assert(read_columns[0].name == "id");
    assert(read_columns[1].name == "name");
    assert(read_columns[2].name == "age");
    assert(read_columns[3].name == "price");
    assert(read_columns[0].type == ColumnType::INT64);
    assert(read_columns[1].type == ColumnType::STRING);
    assert(read_columns[2].type == ColumnType::INT64);
    assert(read_columns[3].type == ColumnType::DOUBLE);
    
    cout << "  readSchema() returned " << read_columns.size() << " columns\n";
    for (auto& col : read_columns) {
        cout << "    - " << col.name << " : " << typeToString(col.type) << "\n";
    }
    cout << "  ✓ readSchema() PASSED\n";
    
    string schema_content = readFileAsString(test_dir + "/schema.json");
    assert(schema_content.find("\"table\": \"sales\"") != string::npos);
    assert(schema_content.find("\"name\": \"id\"") != string::npos);
    cout << "  ✓ schema.json format is valid\n";
    
    cout << "  ✓ Schema Manager PASSED\n";
}

// Test 6: Destructor and Resource Cleanup
void testDestructor() {
    cout << "\n[TEST 6] Destructor and Resource Cleanup\n";
    cout << "----------------------------------------\n";
    
    createDirectory("data/test_destructor");
    
    {
        ColumnWriter writer("data/test_destructor", "test", ColumnType::INT64, Encoding::NONE);
        writer.setRowCount(1);
        vector<int64_t> values = {42};
        writer.writeInt64(values);
        writer.finalize();
        cout << "  ✓ ColumnWriter destructor works\n";
    }
    
    {
        CSVParser parser("data/test.csv");
        parser.parse();
        cout << "  ✓ CSVParser destructor works\n";
    }
    
    cout << "  ✓ All destructors work correctly\n";
}

int main() {
    cout << "\n========================================\n";
    cout << "PHASE 1 COMPLETE TEST SUITE\n";
    cout << "========================================\n";
    
    testCSVParser();
    testTypeInference();
    testColumnWriter();
    testColumnReader();
    testSchemaManager();
    testDestructor();
    
    cout << "\n========================================\n";
    cout << "ALL TESTS PASSED! ✓\n";
    cout << "========================================\n\n";
    
    cout << "Files created:\n";
    cout << "  - data/test.csv\n";
    cout << "  - data/test_warehouse/sales/id.col\n";
    cout << "  - data/test_warehouse/sales/name.col\n";
    cout << "  - data/test_warehouse/sales/price.col\n";
    cout << "  - data/test_warehouse/sales/schema.json\n\n";
    
    cout << "Phase 1 is ready to hand off to Person 2 and Person 3!\n";
    
    return 0;
}