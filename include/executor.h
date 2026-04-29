#ifndef EXECUTOR_H
#define EXECUTOR_H

#include <string>
#include <vector>
#include <unordered_map>
#include <chrono>
#include "query_parser.h"
#include "predicate.h"
#include "schema.h"
#include "common.h"

class ColumnReader;

class Executor {
public:
    // Main entry point. Called by the shell after parsing and predicate evaluation.
    //
    // Parameters:
    //   plan        - the parsed query plan (SELECTs, WHERE, GROUP BY)
    //   bitmap      - bitmap[i] = true if row i passes the WHERE filter
    //                  If empty, all rows pass (no WHERE clause).
    //   readers     - array of open ColumnReader*, one per column in colNames
    //                  Readers are positioned at row 0 and ready to iterate.
    //   colNames    - names of columns that were opened (from plan.neededColumns())
    //   schema      - the full table schema (needed for SELECT * column listing)
    //   tableDir    - path to the table directory
    //
    // Returns:
    //   true if query executed successfully, false on error
    static bool run(const QueryPlan& plan,
                    const Bitmap& bitmap,
                    std::vector<ColumnReader*>& readers,
                    const std::vector<std::string>& colNames,
                    const std::vector<SchemaColumn>& schema,
                    const std::string& tableDir);

private:
    // Find which index in readers[] corresponds to a given column name
    static int findColumnIndex(const std::string& colName,
                               const std::vector<std::string>& colNames);

    // Map from column name to its SchemaColumn (for type info)
    static const SchemaColumn* findSchemaCol(const std::string& colName,
                                              const std::vector<SchemaColumn>& schema);

    // CASE 1: SELECT * (point lookup — reads ALL columns, reconstructs rows)
    static bool executeSelectStar(const QueryPlan& plan,
                                  const Bitmap& bitmap,
                                  std::vector<ColumnReader*>& readers,
                                  const std::vector<std::string>& colNames,
                                  const std::vector<SchemaColumn>& schema);

    // CASE 2: Plain SELECT col1, col2 (no aggregates, no GROUP BY)
    static bool executePlainSelect(const QueryPlan& plan,
                                   const Bitmap& bitmap,
                                   std::vector<ColumnReader*>& readers,
                                   const std::vector<std::string>& colNames);

    // CASE 3: Aggregates WITHOUT GROUP BY
    static bool executeAggregate(const QueryPlan& plan,
                                 const Bitmap& bitmap,
                                 std::vector<ColumnReader*>& readers,
                                 const std::vector<std::string>& colNames);

    // CASE 4: GROUP BY with aggregates
    static bool executeGroupBy(const QueryPlan& plan,
                               const Bitmap& bitmap,
                               std::vector<ColumnReader*>& readers,
                               const std::vector<std::string>& colNames,
                               const std::vector<SchemaColumn>& schema);

    // ── Accumulator for a single aggregate expression ──
    struct AggState {
        AggFunc func;
        double sum;
        int64_t count;
        double minVal;
        double maxVal;

        AggState(AggFunc f);
        void update(double val);
        void updateFromColumnValue(const ColumnValue& cv);
        double result() const;
    };

    // Print a ColumnValue with proper formatting based on type
    static void printColumnValue(const ColumnValue& cv, ColumnType type);
    
    // Convert AggFunc to display name
    static std::string aggFuncName(AggFunc f);

    // Check if query has any aggregate functions
    static bool hasAggregates(const QueryPlan& plan);
    
    // Get the row count by checking any open reader
    static uint64_t getRowCount(std::vector<ColumnReader*>& readers);
    
    // Count rows that pass the filter
    static uint64_t countPassingRows(const Bitmap& bitmap);
    
    // Calculate bytes read from disk
    static size_t calculateBytesRead(const std::vector<std::string>& colNames,
                                     const std::vector<SchemaColumn>& schema,
                                     uint64_t rowCount);
};

#endif // EXECUTOR_H