#include "../include/executor.h"
#include "../include/column_reader.h"
#include <iostream>
#include <iomanip>
#include <cmath>

// ============================================================
// AggState implementation
// ============================================================
Executor::AggState::AggState(AggFunc f)
    : func(f), sum(0.0), count(0), minVal(1e18), maxVal(-1e18) {}

void Executor::AggState::update(double val) {
    switch (func) {
        case AggFunc::COUNT: count++; break;
        case AggFunc::SUM:   sum += val; break;
        case AggFunc::AVG:   sum += val; count++; break;
        case AggFunc::MIN:   if (val < minVal) minVal = val; break;
        case AggFunc::MAX:   if (val > maxVal) maxVal = val; break;
        default: break;
    }
}

void Executor::AggState::updateFromColumnValue(const ColumnValue& cv) {
    double val = 0.0;
    if (std::holds_alternative<int64_t>(cv))
        val = static_cast<double>(std::get<int64_t>(cv));
    else if (std::holds_alternative<double>(cv))
        val = std::get<double>(cv);
    else if (std::holds_alternative<std::string>(cv)) {
        // Try to parse string as double for aggregates (fallback)
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
        case AggFunc::MIN:   return minVal;
        case AggFunc::MAX:   return maxVal;
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

size_t Executor::calculateBytesRead(const std::vector<std::string>& colNames,
                                     const std::vector<SchemaColumn>& schema,
                                     uint64_t rowCount) {
    size_t bytes = 0;
    for (const auto& colName : colNames) {
        const SchemaColumn* sc = findSchemaCol(colName, schema);
        if (!sc) continue;
        switch (sc->type) {
            case ColumnType::INT32:  bytes += rowCount * 4; break;
            case ColumnType::INT64:  bytes += rowCount * 8; break;
            case ColumnType::DOUBLE: bytes += rowCount * 8; break;
            case ColumnType::STRING: bytes += rowCount * 4; break; // rough estimate
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
    
    // Count total rows
    uint64_t rowCount = getRowCount(readers);

    // ── Route to the correct execution path ──
    if (plan.select_star) {
        success = executeSelectStar(plan, bitmap, readers, colNames, schema);
    } else if (!plan.groupby.empty()) {
        success = executeGroupBy(plan, bitmap, readers, colNames, schema);
    } else if (hasAggregates(plan)) {
        success = executeAggregate(plan, bitmap, readers, colNames);
    } else {
        success = executePlainSelect(plan, bitmap, readers, colNames);
    }

    // Stop timing and print stats
    auto end = std::chrono::high_resolution_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    if (success) {
        std::cout << "(" << ms << " ms)\n";
        // Calculate and print stats
        size_t bytesRead = calculateBytesRead(colNames, schema, rowCount);
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
// This is the point-lookup path that column stores are bad at.
// ============================================================
bool Executor::executeSelectStar(const QueryPlan& plan,
                                 const Bitmap& bitmap,
                                 std::vector<ColumnReader*>& readers,
                                 const std::vector<std::string>& colNames,
                                 const std::vector<SchemaColumn>& schema) {

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

    // We need readers for ALL schema columns, in order.
    // Build a mapping: schema index -> reader index
    std::vector<int> schemaToReader(schema.size(), -1);
    for (size_t i = 0; i < schema.size(); i++) {
        schemaToReader[i] = findColumnIndex(schema[i].name, colNames);
    }

    uint64_t rowCount = getRowCount(readers);
    size_t printed = 0;

    for (uint64_t row = 0; row < rowCount; row++) {
        // Check bitmap
        if (!bitmap.empty() && !bitmap[row]) {
            // Still need to advance all readers past this row
            for (auto* r : readers) {
                if (r->hasNext()) r->next();
            }
            continue;
        }

        // Read and print each column
        for (size_t c = 0; c < schema.size(); c++) {
            int rIdx = schemaToReader[c];
            if (rIdx >= 0 && readers[rIdx]->hasNext()) {
                ColumnValue val = readers[rIdx]->next();
                printColumnValue(val, schema[c].type);
            } else {
                std::cout << "NULL";
            }
            if (c + 1 < schema.size()) std::cout << " | ";
        }
        std::cout << "\n";
        printed++;
    }

    if (printed == 1 && !plan.where.has_where) {
        // Single row, no WHERE — already printed above
    }

    return true;
}

// ============================================================
// CASE 2: Plain SELECT col1, col2 (no aggregates, no GROUP BY)
// Example: SELECT country, price FROM sales WHERE id = 500000
// ============================================================
bool Executor::executePlainSelect(const QueryPlan& plan,
                                  const Bitmap& bitmap,
                                  std::vector<ColumnReader*>& readers,
                                  const std::vector<std::string>& colNames) {

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
    size_t printed = 0;

    for (uint64_t row = 0; row < rowCount; row++) {
        // Check bitmap
        if (!bitmap.empty() && !bitmap[row]) {
            // Advance all readers
            for (auto* r : readers) {
                if (r->hasNext()) r->next();
            }
            continue;
        }

        // Read values for this row from all readers
        std::vector<ColumnValue> rowValues(readers.size());
        for (size_t r = 0; r < readers.size(); r++) {
            if (readers[r]->hasNext()) {
                rowValues[r] = readers[r]->next();
            }
        }

        // Print selected columns
        for (size_t s = 0; s < plan.selects.size(); s++) {
            int rIdx = selectToReader[s];
            if (rIdx >= 0) {
                // We don't know the exact type here, print generically
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
        printed++;
    }

    std::cout << "(" << printed << " rows)\n";
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

    // Single pass over all rows
    for (uint64_t row = 0; row < rowCount; row++) {
        // Read values for this row from all readers first
        std::vector<ColumnValue> rowValues(readers.size());
        for (size_t r = 0; r < readers.size(); r++) {
            if (readers[r]->hasNext()) {
                rowValues[r] = readers[r]->next();
            }
        }

        // Check bitmap
        if (!bitmap.empty() && !bitmap[row]) continue;

        // Update each accumulator
        for (size_t s = 0; s < plan.selects.size(); s++) {
            const auto& sel = plan.selects[s];

            if (sel.agg == AggFunc::COUNT && sel.star) {
                // COUNT(*) — just count the row
                states[s].update(0); // value doesn't matter for COUNT
            } else if (sel.agg != AggFunc::NONE) {
                // SUM(col), AVG(col), MIN(col), MAX(col), COUNT(col)
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
        std::cout << states[s].result();
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
                              const std::vector<SchemaColumn>& schema) {

    // Reset all readers
    for (auto* r : readers) r->reset();

    // Find the GROUP BY column reader index
    int groupByIdx = findColumnIndex(plan.groupby, colNames);
    if (groupByIdx < 0) {
        std::cerr << "Error: GROUP BY column '" << plan.groupby << "' not found\n";
        return false;
    }

    // Find which column type the group-by column is
    ColumnType groupByType = ColumnType::STRING;
    const SchemaColumn* gbSchema = findSchemaCol(plan.groupby, schema);
    if (gbSchema) groupByType = gbSchema->type;

    // Map: select index -> reader index (for aggregate columns)
    std::vector<int> selectToReader(plan.selects.size(), -1);
    for (size_t s = 0; s < plan.selects.size(); s++) {
        if (plan.selects[s].agg != AggFunc::NONE && !plan.selects[s].star) {
            selectToReader[s] = findColumnIndex(plan.selects[s].col, colNames);
        }
    }

    // Hash map: key = group value (as string), value = vector of AggStates
    std::unordered_map<std::string, std::vector<AggState>> groups;
    std::vector<std::string> groupOrder; // preserve insertion order

    // Count aggregate expressions (skip GROUP BY column reference)
    size_t numAggs = 0;
    for (const auto& sel : plan.selects) {
        if (sel.agg != AggFunc::NONE) numAggs++;
    }

    uint64_t rowCount = getRowCount(readers);

    // Single pass over all rows
    for (uint64_t row = 0; row < rowCount; row++) {
        // Read values for this row from all readers
        std::vector<ColumnValue> rowValues(readers.size());
        for (size_t r = 0; r < readers.size(); r++) {
            if (readers[r]->hasNext()) {
                rowValues[r] = readers[r]->next();
            }
        }

        // Check bitmap
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
            if (sel.agg == AggFunc::NONE) continue; // skip GROUP BY column

            if (sel.agg == AggFunc::COUNT && sel.star) {
                groups[key][aggIdx].update(0); // COUNT(*)
            } else if (sel.agg != AggFunc::NONE) {
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
                // GROUP BY column — print the key
                std::cout << key;
            } else {
                // Print aggregate result
                std::cout << groups[key][aggIdx].result();
                aggIdx++;
            }
            if (s + 1 < plan.selects.size()) std::cout << " | ";
        }
        std::cout << "\n";
    }

    std::cout << "(" << groupOrder.size() << " groups)\n";
    return true;
}