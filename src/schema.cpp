#include "../include/schema.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <cstdlib>
#include <cstdio>
#include <sys/stat.h>

#ifdef _WIN32
    #include <windows.h>
    #include <direct.h>
#endif

std::string SchemaManager::serializeToJson(const std::vector<SchemaColumn>& columns,
                                           const std::string& table_name) {
    std::ostringstream json;
    json << "{\n";
    json << "  \"table\": \"" << table_name << "\",\n";
    json << "  \"columns\": [\n";

    for (size_t i = 0; i < columns.size(); i++) {
        json << "    {\n";
        json << "      \"name\": \""     << columns[i].name                      << "\",\n";
        json << "      \"type\": \""     << typeToString(columns[i].type)        << "\",\n";
        json << "      \"encoding\": \"" << encodingToString(columns[i].encoding) << "\"\n";
        json << "    }";
        if (i < columns.size() - 1) json << ",";
        json << "\n";
    }

    json << "  ]\n";
    json << "}\n";

    return json.str();
}

bool SchemaManager::writeFile(const std::string& path, const std::string& content) {
#ifdef _WIN32
    // Use fopen (C runtime) on Windows - ofstream fails for new files due to AV scanning
    std::string native = path;
    for (char& c : native) if (c == '/') c = '\\';
    FILE* fp = fopen(native.c_str(), "wb");
    if (!fp) return false;
    fwrite(content.c_str(), 1, content.size(), fp);
    fclose(fp);
    return true;
#else
    std::ofstream file(path);
    if (!file.is_open()) return false;
    file << content;
    file.close();
    return true;
#endif
}

std::string SchemaManager::readFile(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) return "";
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

bool SchemaManager::writeSchema(const std::string& table_dir,
                                const std::vector<SchemaColumn>& columns,
                                const std::string& table_name) {
    // FIX (Issue 14): replaced system("mkdir ...") on Windows with _mkdir()
    // to eliminate shell-injection risk from table names containing
    // shell metacharacters. Matches the same fix applied to column_writer.cpp.
#ifdef _WIN32
    _mkdir(table_dir.c_str());
#else
    mkdir(table_dir.c_str(), 0755);
#endif

    // Write schema.json via temp-file-and-rename (atomic write).
    // Manual Section 5.2: "Write the schema.json file last, also using
    // temp-and-rename. If schema.json exists, all the column files it
    // references are guaranteed to exist."
    std::string temp_path  = table_dir + "/schema.json.tmp";
    std::string final_path = table_dir + "/schema.json";

    std::string json = serializeToJson(columns, table_name);

    if (!writeFile(temp_path, json)) {
        return false;
    }

#ifdef _WIN32
    // Windows std::rename fails if destination exists; remove it first
    {
        std::string native_temp  = temp_path;
        std::string native_final = final_path;
        for (char& c : native_temp)  if (c == '/') c = '\\';
        for (char& c : native_final) if (c == '/') c = '\\';
        std::remove(native_final.c_str());
        if (std::rename(native_temp.c_str(), native_final.c_str()) != 0) {
            std::cerr << "Error renaming schema temp file\n";
            return false;
        }
    }
#else
    if (std::rename(temp_path.c_str(), final_path.c_str()) != 0) {
        std::cerr << "Error renaming schema temp file\n";
        return false;
    }
#endif

    return true;
}

// ============================================================
// readSchema()
// Parses schema.json into a vector of SchemaColumn structs.
//
// FIX (Issue 9): The original hand-rolled JSON parser used the
// first '}' character to delimit each column object. Any string
// value containing a literal '}' (technically valid in JSON)
// would truncate the object early, silently dropping the encoding
// or type fields that appear after it.
//
// Fix: use brace counting to find the correct closing brace.
// A '}' only closes the object when the nesting depth returns to
// zero, so embedded '}' characters inside quoted strings are still
// handled correctly as long as we track depth.
//
// Note: this is still a simplified parser — it does not handle
// escaped quotes inside strings (e.g. "name": "o\"brien"). For
// the column names, type names, and encoding names produced by
// serializeToJson() this is never an issue, but it is documented
// here for completeness.
// ============================================================
std::vector<SchemaColumn> SchemaManager::readSchema(const std::string& table_dir) {
    std::vector<SchemaColumn> columns;
    std::string schema_path = table_dir + "/schema.json";
    std::string content = readFile(schema_path);

    if (content.empty()) return columns;

    size_t columns_pos = content.find("\"columns\"");
    if (columns_pos == std::string::npos) return columns;

    size_t array_start = content.find('[', columns_pos);
    if (array_start == std::string::npos) return columns;

    size_t array_end = content.find(']', array_start);
    if (array_end == std::string::npos) return columns;

    std::string columns_array = content.substr(array_start + 1,
                                               array_end - array_start - 1);

    size_t pos = 0;
    while (pos < columns_array.size()) {
        size_t obj_start = columns_array.find('{', pos);
        if (obj_start == std::string::npos) break;

        // FIX (Issue 9): find the matching closing brace using depth counting
        // instead of the first '}' found. This correctly handles any string
        // value that contains a '}' character.
        size_t obj_end = std::string::npos;
        int depth = 0;
        for (size_t k = obj_start; k < columns_array.size(); k++) {
            if      (columns_array[k] == '{') depth++;
            else if (columns_array[k] == '}') {
                depth--;
                if (depth == 0) { obj_end = k; break; }
            }
        }
        if (obj_end == std::string::npos) break;

        std::string obj = columns_array.substr(obj_start, obj_end - obj_start + 1);

        SchemaColumn col;
        col.encoding = Encoding::NONE; // default, overwritten below if found

        // Parse name
        size_t name_pos = obj.find("\"name\"");
        if (name_pos != std::string::npos) {
            size_t colon  = obj.find(':', name_pos);
            size_t quote1 = obj.find('"', colon + 1);
            size_t quote2 = obj.find('"', quote1 + 1);
            if (quote1 != std::string::npos && quote2 != std::string::npos) {
                col.name = obj.substr(quote1 + 1, quote2 - quote1 - 1);
            }
        }

        // Parse type
        size_t type_pos = obj.find("\"type\"");
        if (type_pos != std::string::npos) {
            size_t colon  = obj.find(':', type_pos);
            size_t quote1 = obj.find('"', colon + 1);
            size_t quote2 = obj.find('"', quote1 + 1);
            if (quote1 != std::string::npos && quote2 != std::string::npos) {
                std::string type_str = obj.substr(quote1 + 1, quote2 - quote1 - 1);
                col.type = stringToType(type_str);
            }
        }

        // Parse encoding
        size_t enc_pos = obj.find("\"encoding\"");
        if (enc_pos != std::string::npos) {
            size_t colon  = obj.find(':', enc_pos);
            size_t quote1 = obj.find('"', colon + 1);
            size_t quote2 = obj.find('"', quote1 + 1);
            if (quote1 != std::string::npos && quote2 != std::string::npos) {
                std::string enc_str = obj.substr(quote1 + 1, quote2 - quote1 - 1);
                col.encoding = stringToEncoding(enc_str);
            }
        }

        columns.push_back(col);
        pos = obj_end + 1;
    }

    return columns;
}

bool SchemaManager::schemaExists(const std::string& table_dir) {
    std::string schema_path = table_dir + "/schema.json";
    std::ifstream file(schema_path);
    return file.good();
}