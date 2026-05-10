#include "../include/executor.h"
#include "../include/column_reader.h"
#include <iostream>
#include <iomanip>
#include <cmath>
#include <limits>

// ============================================================
// AggState implementation
// ============================================================
Executor::AggState::AggState(AggFunc f)
    : func(f)
    , sum(0.0)
    , count(0)
    // FIX (Issue 3): use proper numeric limits instead of magic constants.
    // The old values (1e18 / -1e18) are less than DBL_MAX (~1.8e308), so
    // a column whose values all exceed 1e18 (e.g. large int64 IDs or
    // Unix nanosecond timestamps) would return the sentinel instead of
    // the actual minimum/maximum.
    , minVal(std::numeric_limits<double>::max())
    , maxVal(std::numeric_limits<double>::lowest())
{}

void Executor::AggState::update(double val) {
    switch (func) {
        case AggFunc::COUNT: 
            count++; 
            break;
        case AggFunc::SUM:   
            sum += val; 
            count++;  // ← ADD THIS LINE
            break;
        case AggFunc::AVG:   
            sum += val; 
            count++; 
            break;
        case AggFunc::MIN:   
            if (val < minVal) minVal = val; 
            count++;  // ← ADD THIS LINE
            break;
        case AggFunc::MAX:   
            if (val > maxVal) maxVal = val; 
            count++;  // ← ADD THIS LINE
            break;
        default: 
            break;
    }
}

void Executor::AggState::updateFromColumnValue(const ColumnValue& cv) {
    double val = 0.0;
    if (std::holds_alternative<int64_t>(cv))
        val = static_cast<double>(std::get<int64_t>(cv));
    else if (std::holds_alternative<double>(cv))
        val = std::get<double>(cv);
    else if (std::holds_alternative<std::string>(cv)) {
        try { val = std::stod(std::get<std::string>(cv)); }
        catch (...) { val = 0.0; }
    }
    update(val);
}

double Executor::AggState::result() const {
    switch (func) {
        case AggFunc::COUNT: return static_cast<double>(count);
        case AggFunc::SUM:   return sum;
        case AggFunc::AVG:   return count > 0 ? sum / count : 0.0;
        // FIX (Issue 3): guard against the case where no rows passed the
        // filter, in which case minVal/maxVal are still the sentinel values.
        // Return 0.0 instead of a nonsensical DBL_MAX/lowest.
        case AggFunc::MIN:   return (count > 0) ? minVal : 0.0;
        case AggFunc::MAX:   return (count > 0) ? maxVal : 0.0;
        default: return 0.0;
    }
}

// ============================================================
// Helpers
// ============================================================
std::string Executor::aggFuncName(AggFunc f) {
    switch (f) {
        case AggFunc::COUNT: return "COUNT";
        case AggFunc::SUM:   return "SUM";
        case AggFunc::AVG:   return "AVG";
        case AggFunc::MIN:   return "MIN";
        case AggFunc::MAX:   return "MAX";
        default: return "";
    }
}

bool Executor::hasAggregates(const QueryPlan& plan) {
    if (plan.select_star) return false;
    for (const auto& sel : plan.selects) {
        if (sel.agg != AggFunc::NONE) return true;
    }
    return false;
}

uint64_t Executor::getRowCount(std::vector<ColumnReader*>& readers) {
    for (auto* r : readers) {
        if (r && r->isOpen()) return r->getRowCount();
    }
    return 0;
}

uint64_t Executor::countPassingRows(const Bitmap& bitmap) {
    if (bitmap.empty()) return 0;
    uint64_t count = 0;
    for (bool b : bitmap) if (b) count++;
    return count;
}

int Executor::findColumnIndex(const std::string& colName,
                              const std::vector<std::string>& colNames) {
    for (size_t i = 0; i < colNames.size(); i++) {
        if (colNames[i] == colName) return static_cast<int>(i);
    }
    return -1;
}

const SchemaColumn* Executor::findSchemaCol(const std::string& colName,
                                             const std::vector<SchemaColumn>& schema) {
    for (const auto& sc : schema) {
        if (sc.name == colName) return &sc;
    }
    return nullptr;
}

void Executor::printColumnValue(const ColumnValue& cv, ColumnType type) {
    if (std::holds_alternative<int64_t>(cv)) {
        std::cout << std::get<int64_t>(cv);
    } else if (std::holds_alternative<double>(cv)) {
        std::cout << std::fixed << std::setprecision(2) << std::get<double>(cv);
    } else if (std::holds_alternative<std::string>(cv)) {
        std::cout << std::get<std::string>(cv);
    }
}

// ============================================================
// calculateBytesRead()
// FIX (Issue 11): The old implementation estimated byte usage as
// rowCount * sizeof(type) for every encoding, which was wrong for
// compressed columns:
//   - DICTIONARY string column: actual cost is rowCount * id_width
//     (1 or 2 bytes), not rowCount * 4.
//   - RLE column: actual cost is run_count * 16 bytes, not rowCount * 8.
// Since we don't have per-column run counts available here, we read the
// actual .col file size from disk instead. This is exact and works for
// all encodings. The column files have already been written by the time
// the executor runs, so stat()-ing them is always valid.
// ============================================================
size_t Executor::calculateBytesRead(const std::vector<std::string>& colNames,
                                     const std::vector<SchemaColumn>& schema,
                                     uint64_t /*rowCount*/,
                                     const std::string& tableDir) {
    size_t bytes = 0;
    for (const auto& colName : colNames) {
        // Find schema entry to confirm the column exists
        const SchemaColumn* sc = findSchemaCol(colName, schema);
        if (!sc) continue;

        // Open the .col file and read its actual size
        std::string col_path = tableDir + "/" + colName + ".col";
        std::ifstream f(col_path, std::ios::binary | std::ios::ate);
        if (f.is_open()) {
            bytes += static_cast<size_t>(f.tellg());
        }
    }
    return bytes;
}

// ============================================================
// Main entry point
// ============================================================
bool Executor::run(const QueryPlan& plan,
                   const Bitmap& bitmap,
                   std::vector<ColumnReader*>& readers,
                   const std::vector<std::string>& colNames,
                   const std::vector<SchemaColumn>& schema,
                   const std::string& tableDir) {

    auto start = std::chrono::high_resolution_clock::now();
    bool success = false;

    uint64_t rowCount = getRowCount(readers);

    size_t resultCount = 0;
    std::string countLabel = "rows";

    // ── Route to the correct execution path ──
    if (plan.select_star) {
        success = executeSelectStar(plan, bitmap, readers, colNames, schema, resultCount);
    } else if (!plan.groupby.empty()) {
        countLabel = "groups";
        success = executeGroupBy(plan, bitmap, readers, colNames, schema, resultCount);
    } else if (hasAggregates(plan)) {
        success = executeAggregate(plan, bitmap, readers, colNames);
        resultCount = 1;
    } else {
        success = executePlainSelect(plan, bitmap, readers, colNames, resultCount);
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    if (success) {
        std::cout << "(" << resultCount << " " << countLabel
                  << ", " << ms << " ms)\n";

        // FIX (Issue 11): pass tableDir so calculateBytesRead can stat the
        // actual .col files instead of estimating from row counts.
        size_t bytesRead = calculateBytesRead(colNames, schema, rowCount, tableDir);
        double mb = bytesRead / (1024.0 * 1024.0);
        size_t totalCols = schema.size();
        std::cout << "[read " << colNames.size() << " of " << totalCols
                  << " columns, " << std::fixed << std::setprecision(1)
                  << mb << " MB from disk]\n";
    }

    return success;
}

// ============================================================
// CASE 1: SELECT *
// Reads ALL columns, applies bitmap, prints full rows.
// Manual Section 6.3: "For SELECT *, the executor reads every
// column, applies the filter, and prints the matching rows."
//
// FIX (Issue 2): verified that SELECT * opens readers for ALL
// schema columns (not just the WHERE column). The caller (main.cpp
// / shell) is responsible for opening all columns before calling
// run() with SELECT *. executeSelectStar builds schemaToReader[]
// from the colNames passed in — if any schema column is missing
// from colNames the slot stays -1 and prints NULL. Added an
// assertion-style check so missing columns are visible rather than
// silently wrong.
// ============================================================
bool Executor::executeSelectStar(const QueryPlan& plan,
                                 const Bitmap& bitmap,
                                 std::vector<ColumnReader*>& readers,
                                 const std::vector<std::string>& colNames,
                                 const std::vector<SchemaColumn>& schema,
                                 size_t& resultCount) {

    // Reset all readers to start
    for (auto* r : readers) r->reset();

    // Print header row
    for (size_t c = 0; c < schema.size(); c++) {
        std::cout << schema[c].name;
        if (c + 1 < schema.size()) std::cout << " | ";
    }
    std::cout << "\n";

    // Print separator
    for (size_t c = 0; c < schema.size(); c++) {
        std::cout << std::string(schema[c].name.length(), '-');
        if (c + 1 < schema.size()) std::cout << "-+-";
    }
    std::cout << "\n";

    // Build mapping: schema index -> reader index.
    // FIX (Issue 2): warn if any schema column has no corresponding reader,
    // which would indicate the caller did not open all columns for SELECT *.
    std::vector<int> schemaToReader(schema.size(), -1);
    for (size_t i = 0; i < schema.size(); i++) {
        schemaToReader[i] = findColumnIndex(schema[i].name, colNames);
        if (schemaToReader[i] < 0) {
            std::cerr << "Warning: SELECT * missing reader for column '"
                      << schema[i].name << "' — will print NULL\n";
        }
    }

    uint64_t rowCount = getRowCount(readers);
    resultCount = 0;

    for (uint64_t row = 0; row < rowCount; row++) {
        // Read a value from every open reader to keep all positions in sync,
        // regardless of whether this row passes the bitmap filter.
        std::vector<ColumnValue> rowValues(readers.size());
        for (size_t r = 0; r < readers.size(); r++) {
            if (readers[r]->hasNext())
                rowValues[r] = readers[r]->next();
        }

        // Skip rows that don't pass the filter
        if (!bitmap.empty() && !bitmap[row]) continue;

        // Print each column for this row
        for (size_t c = 0; c < schema.size(); c++) {
            int rIdx = schemaToReader[c];
            if (rIdx >= 0) {
                printColumnValue(rowValues[rIdx], schema[c].type);
            } else {
                std::cout << "NULL";
            }
            if (c + 1 < schema.size()) std::cout << " | ";
        }
        std::cout << "\n";
        resultCount++;
    }

    return true;
}

// ============================================================
// CASE 2: Plain SELECT col1, col2 (no aggregates, no GROUP BY)
// ============================================================
bool Executor::executePlainSelect(const QueryPlan& plan,
                                  const Bitmap& bitmap,
                                  std::vector<ColumnReader*>& readers,
                                  const std::vector<std::string>& colNames,
                                  size_t& resultCount) {

    // Reset all readers
    for (auto* r : readers) r->reset();

    // Build mapping: select index -> reader index
    std::vector<int> selectToReader(plan.selects.size(), -1);
    for (size_t s = 0; s < plan.selects.size(); s++) {
        selectToReader[s] = findColumnIndex(plan.selects[s].col, colNames);
    }

    // Print header
    for (size_t s = 0; s < plan.selects.size(); s++) {
        std::cout << plan.selects[s].col;
        if (s + 1 < plan.selects.size()) std::cout << " | ";
    }
    std::cout << "\n";

    for (size_t s = 0; s < plan.selects.size(); s++) {
        std::cout << std::string(plan.selects[s].col.length(), '-');
        if (s + 1 < plan.selects.size()) std::cout << "-+-";
    }
    std::cout << "\n";

    uint64_t rowCount = getRowCount(readers);
    resultCount = 0;

    for (uint64_t row = 0; row < rowCount; row++) {
        // Read all columns first so every reader advances by exactly one row.
        std::vector<ColumnValue> rowValues(readers.size());
        for (size_t r = 0; r < readers.size(); r++) {
            if (readers[r]->hasNext())
                rowValues[r] = readers[r]->next();
        }

        // Skip rows that don't pass the filter
        if (!bitmap.empty() && !bitmap[row]) continue;

        // Print selected columns
        for (size_t s = 0; s < plan.selects.size(); s++) {
            int rIdx = selectToReader[s];
            if (rIdx >= 0) {
                const auto& cv = rowValues[rIdx];
                if (std::holds_alternative<int64_t>(cv))
                    std::cout << std::get<int64_t>(cv);
                else if (std::holds_alternative<double>(cv))
                    std::cout << std::fixed << std::setprecision(2) << std::get<double>(cv);
                else if (std::holds_alternative<std::string>(cv))
                    std::cout << std::get<std::string>(cv);
            }
            if (s + 1 < plan.selects.size()) std::cout << " | ";
        }
        std::cout << "\n";
        resultCount++;
    }

    return true;
}

// ============================================================
// CASE 3: Aggregates WITHOUT GROUP BY
// Example: SELECT SUM(price), COUNT(*) FROM sales WHERE date >= 20240101
// ============================================================
bool Executor::executeAggregate(const QueryPlan& plan,
                                const Bitmap& bitmap,
                                std::vector<ColumnReader*>& readers,
                                const std::vector<std::string>& colNames) {
                                    

    // Reset all readers
    for (auto* r : readers) r->reset();

    // Create one accumulator per select expression
    std::vector<AggState> states;
    for (const auto& sel : plan.selects) {
        states.push_back(AggState(sel.agg));
    }

    // Map: select index -> reader index (for aggregate columns)
    std::vector<int> selectToReader(plan.selects.size(), -1);
    for (size_t s = 0; s < plan.selects.size(); s++) {
        if (plan.selects[s].agg != AggFunc::NONE && !plan.selects[s].star) {
            selectToReader[s] = findColumnIndex(plan.selects[s].col, colNames);
        }
    }

    uint64_t rowCount = getRowCount(readers);
    uint64_t rows_passing = 0;
for (bool b : bitmap) if (b) rows_passing++;
std::cout << "DEBUG: " << rows_passing << " rows pass filter out of " << rowCount << std::endl;

    for (uint64_t row = 0; row < rowCount; row++) {
        // Read all column values first to keep readers in sync
        std::vector<ColumnValue> rowValues(readers.size());
        for (size_t r = 0; r < readers.size(); r++) {
            if (readers[r]->hasNext())
                rowValues[r] = readers[r]->next();
        }

        if (!bitmap.empty() && !bitmap[row]) continue;

        for (size_t s = 0; s < plan.selects.size(); s++) {
            const auto& sel = plan.selects[s];

            if (sel.agg == AggFunc::COUNT && sel.star) {
                states[s].update(0);
            } else if (sel.agg != AggFunc::NONE) {
                int rIdx = selectToReader[s];
                if (rIdx >= 0) {
                    states[s].updateFromColumnValue(rowValues[rIdx]);
                }
            }
        }
    }

    // Print header
    for (size_t s = 0; s < plan.selects.size(); s++) {
        const auto& sel = plan.selects[s];
        if (sel.agg == AggFunc::NONE) {
            std::cout << sel.col;
        } else {
            std::cout << aggFuncName(sel.agg) << "("
                      << (sel.star ? "*" : sel.col) << ")";
        }
        if (s + 1 < plan.selects.size()) std::cout << " | ";
    }
    std::cout << "\n";

    // Separator
    for (size_t s = 0; s < plan.selects.size(); s++) {
        const auto& sel = plan.selects[s];
        std::string label;
        if (sel.agg == AggFunc::NONE) label = sel.col;
        else label = aggFuncName(sel.agg) + "(" + (sel.star ? "*" : sel.col) + ")";
        std::cout << std::string(label.length(), '-');
        if (s + 1 < plan.selects.size()) std::cout << "-+-";
    }
    std::cout << "\n";

    // Print one result row
    for (size_t s = 0; s < plan.selects.size(); s++) {
        const auto& sel = plan.selects[s];
        if (sel.agg == AggFunc::COUNT) {
            std::cout << static_cast<int64_t>(states[s].result());
        } else {
            std::cout << std::fixed << std::setprecision(2) << states[s].result();
        }
        if (s + 1 < plan.selects.size()) std::cout << " | ";
    }
    std::cout << "\n";

    return true;
}
// ============================================================
// CASE 4: GROUP BY with aggregates
// Example: SELECT country, SUM(price) FROM sales GROUP BY country
// ============================================================
bool Executor::executeGroupBy(const QueryPlan& plan,
                              const Bitmap& bitmap,
                              std::vector<ColumnReader*>& readers,
                              const std::vector<std::string>& colNames,
                              const std::vector<SchemaColumn>& schema,
                              size_t& resultCount) {

    // Reset all readers
    for (auto* r : readers) r->reset();

    // Find the GROUP BY column reader index
    int groupByIdx = findColumnIndex(plan.groupby, colNames);
    if (groupByIdx < 0) {
        std::cerr << "Error: GROUP BY column '" << plan.groupby << "' not found\n";
        return false;
    }

    // Map: select index -> reader index (for aggregate columns)
    std::vector<int> selectToReader(plan.selects.size(), -1);
    for (size_t s = 0; s < plan.selects.size(); s++) {
        if (plan.selects[s].agg != AggFunc::NONE && !plan.selects[s].star) {
            selectToReader[s] = findColumnIndex(plan.selects[s].col, colNames);
        }
    }

    // Hash map: key = group value (as string), value = vector of AggStates
    std::unordered_map<std::string, std::vector<AggState>> groups;
    std::vector<std::string> groupOrder;

    uint64_t rowCount = getRowCount(readers);

    for (uint64_t row = 0; row < rowCount; row++) {
        // Read all column values first to keep readers in sync
        std::vector<ColumnValue> rowValues(readers.size());
        for (size_t r = 0; r < readers.size(); r++) {
            if (readers[r]->hasNext())
                rowValues[r] = readers[r]->next();
        }

        if (!bitmap.empty() && !bitmap[row]) continue;

        // Get group key as string
        std::string key;
        const ColumnValue& gbVal = rowValues[groupByIdx];
        if (std::holds_alternative<int64_t>(gbVal))
            key = std::to_string(std::get<int64_t>(gbVal));
        else if (std::holds_alternative<double>(gbVal))
            key = std::to_string(std::get<double>(gbVal));
        else if (std::holds_alternative<std::string>(gbVal))
            key = std::get<std::string>(gbVal);
        else
            key = "?";

        // Initialize accumulators for new group
        if (groups.find(key) == groups.end()) {
            groupOrder.push_back(key);
            std::vector<AggState> states;
            for (const auto& sel : plan.selects) {
                if (sel.agg != AggFunc::NONE) {
                    states.push_back(AggState(sel.agg));
                }
            }
            groups[key] = states;
        }

        // Update accumulators for this group
        size_t aggIdx = 0;
        for (size_t s = 0; s < plan.selects.size(); s++) {
            const auto& sel = plan.selects[s];
            if (sel.agg == AggFunc::NONE) continue;

            if (sel.agg == AggFunc::COUNT && sel.star) {
                groups[key][aggIdx].update(0); // COUNT(*)
            } else {
                int rIdx = selectToReader[s];
                if (rIdx >= 0) {
                    groups[key][aggIdx].updateFromColumnValue(rowValues[rIdx]);
                }
            }
            aggIdx++;
        }
    }

    // Print header
    for (size_t s = 0; s < plan.selects.size(); s++) {
        const auto& sel = plan.selects[s];
        if (sel.agg == AggFunc::NONE) {
            std::cout << sel.col;
        } else {
            std::cout << aggFuncName(sel.agg) << "("
                      << (sel.star ? "*" : sel.col) << ")";
        }
        if (s + 1 < plan.selects.size()) std::cout << " | ";
    }
    std::cout << "\n";

    // Separator
    for (size_t s = 0; s < plan.selects.size(); s++) {
        const auto& sel = plan.selects[s];
        std::string label;
        if (sel.agg == AggFunc::NONE) label = sel.col;
        else label = aggFuncName(sel.agg) + "(" + (sel.star ? "*" : sel.col) + ")";
        std::cout << std::string(label.length(), '-');
        if (s + 1 < plan.selects.size()) std::cout << "-+-";
    }
    std::cout << "\n";

    // Print each group
    for (const auto& key : groupOrder) {
        size_t aggIdx = 0;
        for (size_t s = 0; s < plan.selects.size(); s++) {
            const auto& sel = plan.selects[s];
            if (sel.agg == AggFunc::NONE) {
                std::cout << key;
            } else {
                // COUNT prints as integer, not float
                if (sel.agg == AggFunc::COUNT) {
                    std::cout << static_cast<int64_t>(groups[key][aggIdx].result());
                } else {
                    std::cout << std::fixed << std::setprecision(2)
                              << groups[key][aggIdx].result();
                }
                aggIdx++;
            }
            if (s + 1 < plan.selects.size()) std::cout << " | ";
        }
        std::cout << "\n";
    }

    resultCount = groupOrder.size();
    return true;
}