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
};

} // namespace knk
