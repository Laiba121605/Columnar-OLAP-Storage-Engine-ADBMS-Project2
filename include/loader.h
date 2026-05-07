#ifndef LOADER_H
#define LOADER_H

#include "common.h"
#include <string>

// No changes needed to Loader header
class Loader {
public:
    static bool load(const std::string& csv_file,
                     const std::string& table_name,
                     const std::string& warehouse_path);
};

#endif
