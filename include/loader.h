#ifndef LOADER_H
#define LOADER_H

#include "common.h"
#include <string>

class Loader {
public:
    static bool load(const std::string& csv_file, 
                     const std::string& table_name,
                     const std::string& warehouse_path);
};

#endif