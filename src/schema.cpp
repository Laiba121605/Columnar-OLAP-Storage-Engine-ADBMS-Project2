#include "../include/schema.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <cstdlib>

#ifdef _WIN32
    #include <windows.h>
#endif

std::string SchemaManager::serializeToJson(const std::vector<SchemaColumn>& columns,
                                           const std::string& table_name) {
    std::ostringstream json;
    json << "{\n";
    json << "  \"table\": \"" << table_name << "\",\n";
    json << "  \"columns\": [\n";
    
    for (size_t i = 0; i < columns.size(); i++) {
        json << "    {\n";
        json << "      \"name\": \"" << columns[i].name << "\",\n";
        json << "      \"type\": \"" << typeToString(columns[i].type) << "\",\n";
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
    std::ofstream file(path);
    if (!file.is_open()) {
        return false;
    }
    file << content;
    file.close();
    return true;
}

std::string SchemaManager::readFile(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return "";
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

bool SchemaManager::writeSchema(const std::string& table_dir,
                               const std::vector<SchemaColumn>& columns,
                               const std::string& table_name) {
    
    // Create directory if it doesn't exist
#ifdef _WIN32
    std::string cmd = "mkdir \"" + table_dir + "\" 2> nul";
    system(cmd.c_str());
#else
    mkdir(table_dir.c_str(), 0755);
#endif
    
    std::string schema_path = table_dir + "/schema.json";
    std::string json = serializeToJson(columns, table_name);
    
    return writeFile(schema_path, json);
}

std::vector<SchemaColumn> SchemaManager::readSchema(const std::string& table_dir) {
    std::vector<SchemaColumn> columns;
    std::string schema_path = table_dir + "/schema.json";
    std::string content = readFile(schema_path);
    
    if (content.empty()) {
        return columns;
    }
    
    // Simple JSON parsing
    size_t columns_pos = content.find("\"columns\"");
    if (columns_pos == std::string::npos) {
        return columns;
    }
    
    size_t array_start = content.find('[', columns_pos);
    if (array_start == std::string::npos) {
        return columns;
    }
    
    size_t array_end = content.find(']', array_start);
    if (array_end == std::string::npos) {
        return columns;
    }
    
    std::string columns_array = content.substr(array_start + 1, array_end - array_start - 1);
    
    size_t pos = 0;
    while (pos < columns_array.size()) {
        size_t obj_start = columns_array.find('{', pos);
        if (obj_start == std::string::npos) break;
        
        size_t obj_end = columns_array.find('}', obj_start);
        if (obj_end == std::string::npos) break;
        
        std::string obj = columns_array.substr(obj_start, obj_end - obj_start + 1);
        
        SchemaColumn col;
        col.encoding = Encoding::NONE;
        
        // Parse name
        size_t name_pos = obj.find("\"name\"");
        if (name_pos != std::string::npos) {
            size_t colon = obj.find(':', name_pos);
            size_t quote1 = obj.find('"', colon + 1);
            size_t quote2 = obj.find('"', quote1 + 1);
            if (quote1 != std::string::npos && quote2 != std::string::npos) {
                col.name = obj.substr(quote1 + 1, quote2 - quote1 - 1);
            }
        }
        
        // Parse type
        size_t type_pos = obj.find("\"type\"");
        if (type_pos != std::string::npos) {
            size_t colon = obj.find(':', type_pos);
            size_t quote1 = obj.find('"', colon + 1);
            size_t quote2 = obj.find('"', quote1 + 1);
            if (quote1 != std::string::npos && quote2 != std::string::npos) {
                std::string type_str = obj.substr(quote1 + 1, quote2 - quote1 - 1);
                col.type = stringToType(type_str);
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