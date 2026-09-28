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

#include <vector>

#include "kernel/aggregate.hpp"
#include "kernel/assertion.hpp"
#include "kernel/catalog.hpp"
#include "kernel/index_manager.hpp"
#include "kernel/query.hpp"

namespace knk {

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
    QueryResult execute(const Query &query, const std::vector<Assertion> &assertions, const IndexManager &index_manager,
                        const Catalog &catalog) const;

    // Aggregates the rows an AggregateQuery::selection matches, in a single streaming pass: rows are
    // folded into their group as they are visited and never materialized as a result set, so memory is
    // bounded by the number of groups rather than by the number of matching rows. (count_distinct is
    // the one exception -- it must remember the values it has seen.)
    //
    // Throws std::runtime_error for the same class of caller mistakes execute() rejects, plus: no
    // aggregations, more than MAX_GROUP_BY_FIELDS group-by fields, a bucket field without a positive
    // width, a selection carrying row-shaping fields that mean nothing here, and -- at evaluation time
    // -- an aggregate producing more groups than its cap allows.
    AggregateResult aggregate(const AggregateQuery &query, const std::vector<Assertion> &assertions,
                              const IndexManager &index_manager, const Catalog &catalog) const;
};

} // namespace knk
