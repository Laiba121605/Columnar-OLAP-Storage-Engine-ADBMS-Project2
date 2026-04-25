#ifndef SCHEMA_H
#define SCHEMA_H

#include "common.h"
#include <string>
#include <vector>

struct SchemaColumn {
    std::string name;
    ColumnType  type;
    Encoding    encoding;
};

class SchemaManager {
public:
    static bool writeSchema(const std::string& table_dir,
                            const std::vector<SchemaColumn>& columns,
                            const std::string& table_name);
    static std::vector<SchemaColumn> readSchema(const std::string& table_dir);
    static bool schemaExists(const std::string& table_dir);

private:
    static std::string serializeToJson(const std::vector<SchemaColumn>& columns,
                                       const std::string& table_name);
    static bool writeFile(const std::string& path, const std::string& content);
    static std::string readFile(const std::string& path);
};

#endif // SCHEMA_H
