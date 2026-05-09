#pragma once
#include "common.h"
#include "schema.h"
#include "query_parser.h"
#include "column_reader.h"
#include <vector>
#include <string>
#include <unordered_map>
#include <chrono>
#include <fstream>   // needed by calculateBytesRead for file size lookup

// Bitmap: one bool per row. true = row passes the WHERE filter.
using Bitmap = std::vector<bool>;

// ============================================================
// Executor
// Runs a parsed QueryPlan against a set of open ColumnReaders.
//
// FIX (Issue 11): calculateBytesRead() now takes tableDir so it
// can stat the actual .col files for accurate MB reporting instead
// of estimating from row counts (which was wrong for compressed
// columns).
// ============================================================
class Executor {
public:
    // Aggregate accumulator
    struct AggState {
        AggFunc func;
        double  sum;
        int64_t count;
        double  minVal;
        double  maxVal;

        explicit AggState(AggFunc f);
        void   update(double val);
        void   updateFromColumnValue(const ColumnValue& cv);
        double result() const;
    };

    // Main entry point. Returns false on error.
    // FIX (Issue 11): tableDir added so run() can pass it to
    // calculateBytesRead for accurate disk-usage reporting.
    static bool run(const QueryPlan&                  plan,
             const Bitmap&                     bitmap,
             std::vector<ColumnReader*>&        readers,
             const std::vector<std::string>&    colNames,
             const std::vector<SchemaColumn>&   schema,
             const std::string&                 tableDir);

private:
    // Execution paths
    static bool executeSelectStar  (const QueryPlan& plan,
                             const Bitmap& bitmap,
                             std::vector<ColumnReader*>& readers,
                             const std::vector<std::string>& colNames,
                             const std::vector<SchemaColumn>& schema,
                             size_t& resultCount);

    static bool executePlainSelect (const QueryPlan& plan,
                             const Bitmap& bitmap,
                             std::vector<ColumnReader*>& readers,
                             const std::vector<std::string>& colNames,
                             size_t& resultCount);

    static bool executeAggregate   (const QueryPlan& plan,
                             const Bitmap& bitmap,
                             std::vector<ColumnReader*>& readers,
                             const std::vector<std::string>& colNames);

    static bool executeGroupBy     (const QueryPlan& plan,
                             const Bitmap& bitmap,
                             std::vector<ColumnReader*>& readers,
                             const std::vector<std::string>& colNames,
                             const std::vector<SchemaColumn>& schema,
                             size_t& resultCount);

    // Helpers
    static std::string           aggFuncName(AggFunc f);
    static bool                  hasAggregates(const QueryPlan& plan);
    static uint64_t              getRowCount(std::vector<ColumnReader*>& readers);
    static uint64_t              countPassingRows(const Bitmap& bitmap);
    static int                   findColumnIndex(const std::string& colName,
                                                 const std::vector<std::string>& colNames);
    static const SchemaColumn*   findSchemaCol(const std::string& colName,
                                               const std::vector<SchemaColumn>& schema);
    static void                  printColumnValue(const ColumnValue& cv, ColumnType type);

    // FIX (Issue 11): tableDir parameter added — implementation reads actual
    // file sizes instead of estimating from row counts.
    static size_t calculateBytesRead(const std::vector<std::string>& colNames,
                                     const std::vector<SchemaColumn>& schema,
                                     uint64_t rowCount,
                                     const std::string& tableDir);
};