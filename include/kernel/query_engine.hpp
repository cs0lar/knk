#pragma once

// Plans and executes a Query (Phases 10-11; see AGENTS.md's "Query Engine" section). This is the one
// place query semantics compose: every filter the existing KnowledgeKernel query methods apply
// individually is decomposed here into fields that can be combined, and the methods themselves stay
// the documented way to ask the simple questions.
//
// Read-only and adds no source of truth. It is also deliberately **stateless**: the state a query runs
// against is passed to execute() rather than held as references. Holding references would make
// KnowledgeKernel unsafe to move -- the copies would keep pointing at the moved-from object's vectors
// -- which is a trap worth not setting, since KnowledgeKernel is returned by value in places (see
// benchmarks/query_benchmark.cpp).
//
// It sits above IndexManager in the layering, so IndexManager keeps returning ids only and never
// learns what "current" means.

#include <cstdint>
#include <span>
#include <vector>

#include "kernel/aggregate.hpp"
#include "kernel/assertion.hpp"
#include "kernel/catalog.hpp"
#include "kernel/index_manager.hpp"
#include "kernel/query.hpp"
#include "kernel/query_plan.hpp"
#include "kernel/status_history.hpp"
#include "kernel/vectorized_scan.hpp"

namespace knk {

// Everything a query runs against, passed rather than held -- see the note above on why this engine is
// stateless. Grouped into a struct because Phase 15 added two more inputs and later phases will add
// statistics; a five-parameter call that grows every phase is its own kind of bug.
struct QuerySource {
    std::span<const Assertion> assertions;
    const IndexManager &index_manager;
    const Catalog &catalog;

    // The columnar projection, empty when unavailable (no columns/ at all, a read-only open of a root
    // that never had them, or a store that lags the log). Empty simply means the row path answers.
    ColumnSpans columns;

    // Replayed, effective status per row, parallel to `assertions`. Needed because the columns are a
    // verbatim projection of the log and their status byte is the one the record was *appended* with.
    std::span<const uint8_t> effective_status;

    // How each row's status came to be what it is, which is what an as-of query reconstructs from
    // (Phase 19). Unused unless Query::as_of_commit or as_of_observed is set.
    const StatusHistory &status_history;
};

class QueryEngine {
  public:
    // Throws std::runtime_error for a structurally invalid query: an unknown ir_version, a filter
    // nested past MAX_FILTER_DEPTH, an And/Or with no children, a Not without exactly one, an operand
    // whose Value kind does not match its field, or an unknown status name. Those are caller mistakes,
    // and a rejected query beats a silently reinterpreted one.
    //
    // Everything data-dependent is just a filter that does not match: an over-large limit is capped
    // rather than refused, a query naming ids that were never interned returns nothing, and an
    // ObjectValue comparison against a row whose value is of a different kind (or has no interned value
    // at all) is false rather than an error -- assertion objects are heterogeneous by design.
    QueryResult execute(const Query &query, const QuerySource &source) const;

    // Aggregates the rows an AggregateQuery::selection matches, in a single streaming pass: rows are
    // folded into their group as they are visited and never materialized as a result set, so memory is
    // bounded by the number of groups rather than by the number of matching rows. (count_distinct is
    // the one exception -- it must remember the values it has seen.)
    //
    // Throws std::runtime_error for the same class of caller mistakes execute() rejects, plus: no
    // aggregations, more than MAX_GROUP_BY_FIELDS group-by fields, a bucket field without a positive
    // width, a selection carrying row-shaping fields that mean nothing here, and -- at evaluation time
    // -- an aggregate producing more groups than its cap allows.
    AggregateResult aggregate(const AggregateQuery &query, const QuerySource &source) const;

    // The plan execute() would follow for this query, without running it. Produced by the same call the
    // executor makes, so the two cannot describe different things; cheap because planning reads index
    // bucket sizes rather than index contents.
    QueryPlan explain(const Query &query, const QuerySource &source) const;

    // The ordered row indices a query matches, before paging -- what execute() pages from and what a
    // spill streams. `ordered_prefix` says how many leading rows must actually be in order, so a page
    // costs a partial sort and a spill a full one. Exposed because materializing rows is the caller's
    // decision: a page copies a handful, a spill writes millions without either paying the other's cost.
    std::vector<uint32_t> select_rows(const Query &query, const QuerySource &source, size_t ordered_prefix) const;
};

} // namespace knk
