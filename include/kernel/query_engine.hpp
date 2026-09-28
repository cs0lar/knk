#pragma once

// Plans and executes a Query (Phase 10; see AGENTS.md's "Query Engine" section). This is the one place
// query semantics compose: every filter the existing KnowledgeKernel query methods apply individually
// is decomposed here into fields that can be combined, and the methods themselves stay the documented
// way to ask the simple questions.
//
// Read-only and adds no source of truth. It holds const references to the kernel's in-memory state --
// assertions_ for the rows, IndexManager for candidate selection, Catalog for merge resolution -- and
// writes nothing. It sits above IndexManager in the layering, so IndexManager keeps returning ids
// only and never learns what "current" means.

#include <optional>
#include <vector>

#include "kernel/assertion.hpp"
#include "kernel/catalog.hpp"
#include "kernel/ids.hpp"
#include "kernel/index_manager.hpp"
#include "kernel/query.hpp"

namespace knk {

class QueryEngine {
  public:
    QueryEngine(const std::vector<Assertion> &assertions, const IndexManager &index_manager, const Catalog &catalog);

    // Throws std::runtime_error for a Query whose ir_version this build does not know -- a rejected
    // query is always better than a silently reinterpreted one. Everything else is a filter: an
    // over-large limit is capped rather than refused, and a query matching nothing is an empty result,
    // not an error, consistent with how the existing query methods answer.
    QueryResult execute(const Query &query) const;

  private:
    // The ids worth evaluating, before filtering. Conservative by design in Phase 10: an index is used
    // only where it is known to contain every assertion the query could match, and anything else falls
    // back to scanning assertions_. Selection may change cost, never results -- Query::force_scan
    // exists so tests can assert exactly that. Real (cost-based, statistics-driven) selection is
    // Phase 11/16 work.
    std::vector<AssertionId> candidate_ids(const Query &query, std::optional<EntityId> subject,
                                           std::optional<EntityId> object) const;

    bool matches(const Query &query, const Assertion &assertion, std::optional<EntityId> subject,
                 std::optional<EntityId> object) const;

    const Assertion *find_by_id(AssertionId id) const;

    const std::vector<Assertion> &assertions_;
    const IndexManager &index_manager_;
    const Catalog &catalog_;
};

} // namespace knk
