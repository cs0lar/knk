#pragma once

// What the engine decided to do with a query, and why (Phase 16).
//
// A plan is returned by explain_query and is the same structure the executor follows, not a
// reconstruction of it -- an explanation that can disagree with the execution is worse than none. An
// analytics caller needs to predict what a query will cost before running it, and to see when a change
// to the corpus has changed the plan underneath them.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace knk {

// Where the rows a query examines come from. Every source but the scans is an index, and an index may
// only be chosen when it provably contains every row the query could match -- selection changes cost,
// never results (see QueryEngine).
enum class PlanSource : uint8_t {
    SubjectIndex,
    ObservedTimeIndex,
    ObjectCurrentIndex,
    PredicateCurrentIndex,
    ColumnarScan,
    RowScan,
};

const char *plan_source_name(PlanSource source);

struct PlanOption {
    PlanSource source = PlanSource::RowScan;

    // Rows this source would hand the executor before any filtering. **Exact** for the index sources --
    // a bucket size or a binary search, not a sampled estimate -- and the full row count for a scan.
    size_t rows = 0;

    // Modelled cost in arbitrary units, comparable only against the other options in the same plan. The
    // constants behind it are measured, not guessed; see query_plan.cpp.
    double cost = 0.0;

    // Empty when the option was usable. Otherwise why it could not be chosen -- which is the half of an
    // explanation that tells a caller what to change.
    std::string rejected_because;
};

struct QueryPlan {
    PlanSource chosen = PlanSource::RowScan;

    size_t estimated_rows = 0;
    double estimated_cost = 0.0;

    // Every source the planner weighed, including the rejected ones and the reason.
    std::vector<PlanOption> considered;

    // True when a filter tree will be evaluated per surviving row, which is the cost a caller can most
    // easily avoid by reshaping a query into selectors.
    bool filter_evaluated_per_row = false;

    // Corpus shape the estimates were made against, so a plan can be read months later and still mean
    // something.
    size_t total_rows = 0;
    size_t distinct_subjects = 0;
};

} // namespace knk
