#pragma once

#include <algorithm>

// Chooses which source a query reads its candidate rows from (Phase 16), replacing the fixed heuristics
// Phase 11 shipped.
//
// The choice is cost-based in the literal sense: each usable source is given an exact row count and a
// modelled cost, and the cheapest wins. The row counts are not estimates -- IndexManager can report a
// bucket size or a binary-search position in constant time, so the only thing being modelled is cost
// *per* row, and those constants are measured (see query_plan.cpp).
//
// The rule that matters, and the one Phase 11's heuristics got wrong: an index row costs several times a
// scanned column row, because it is a random access into a record rather than a streamed column. So
// "use an index whenever one applies" is wrong for a common predicate -- scanning beats an index that
// yields a large fraction of the corpus. Nothing here may change *results*: a source is only offered
// when it provably contains every row the query could match.

#include <cstddef>
#include <optional>

#include "kernel/ids.hpp"
#include "kernel/index_manager.hpp"
#include "kernel/query.hpp"
#include "kernel/query_plan.hpp"

namespace knk {

// `subject` and `object` are already resolved through merge redirects. `columns_available` says whether
// the columnar store covers exactly the rows being queried (see columns_usable).
// The tightest upper bound on observed_at the query implies, which is what the observed-time index can
// be driven by. as_of_observed is such a bound: a row observed later was not known at the as-of point.
// Shared between the planner and the executor deliberately -- if the two disagreed about this, the index
// would be costed for one window and read for another.
inline std::optional<Timestamp> observed_upper_bound(const Query &query) {
    if (query.observed_to.has_value() && query.as_of_observed.has_value()) {
        return std::min(*query.observed_to, *query.as_of_observed);
    }
    return query.observed_to.has_value() ? query.observed_to : query.as_of_observed;
}

QueryPlan plan_query(const Query &query, std::optional<EntityId> subject, std::optional<EntityId> object,
                     const IndexManager &index_manager, bool columns_available, size_t total_rows);

} // namespace knk
