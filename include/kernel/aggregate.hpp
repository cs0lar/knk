#pragma once

// The aggregate IR (Phase 12): counts, sums and extrema over groups of assertions. Same philosophy as
// query.hpp -- a closed, typed, serializable description rather than a text language -- and it reuses
// Query wholesale for *which* rows to aggregate, so filters, selectors, bitemporal cutoffs and status
// rules mean exactly what they mean for a row query. There is no second interpretation of "current"
// hiding in here.

#include <cstdint>
#include <optional>
#include <vector>

#include "kernel/query.hpp"
#include "kernel/value.hpp"

namespace knk {

// Cap on distinct groups one aggregate may produce. Exceeding it throws rather than truncating: half
// an aggregate is not a smaller aggregate, it is a wrong one, and a caller that silently received the
// first 10,000 groups of a GROUP BY subject would have no way to know.
constexpr size_t MAX_GROUP_COUNT = 10'000;

// Cap on how many fields one aggregate may group by. Keeps the key small and bounded, for the same
// reason MAX_FILTER_DEPTH bounds a filter.
constexpr size_t MAX_GROUP_BY_FIELDS = 4;

enum class AggregateFunction : uint8_t { Count, CountDistinct, Sum, Min, Max, Avg };

// What an aggregation reads from each row. ObjectValue is the object's *interned Value* rather than its
// id, which is what makes "average salary" expressible; the id targets are there for counting distinct
// subjects or predicates, not for summing.
enum class AggregateTarget : uint8_t {
    ObjectValue,
    Confidence,
    ValidFrom,
    ValidTo,
    ObservedAt,
    Subject,
    Predicate,
    Object,
};

struct Aggregation {
    AggregateFunction function = AggregateFunction::Count;
    AggregateTarget target = AggregateTarget::ObjectValue; // ignored by Count, which counts rows
};

enum class GroupField : uint8_t { Subject, Predicate, Object, Status, ValidFromBucket, ObservedAtBucket };

struct GroupBy {
    GroupField field = GroupField::Subject;

    // Required (> 0) for the bucket fields, ignored otherwise. A bucket is floor(t / width) * width,
    // with floor semantics that stay correct for timestamps before the epoch.
    Timestamp bucket_width = 0;
};

struct AggregateQuery {
    uint32_t ir_version = QUERY_IR_VERSION;

    // Which rows to aggregate. The row-shaping fields (limit, offset, order, newest_first,
    // resolve_names) describe how rows are *returned* and mean nothing for an aggregate, so a
    // selection carrying non-default values for them is rejected rather than quietly ignored.
    Query selection;

    // Empty means one group over everything -- a global aggregate with an empty key.
    std::vector<GroupBy> group_by;

    // At least one; an aggregate with nothing to aggregate is a caller mistake.
    std::vector<Aggregation> aggregations;

    // 0 means MAX_GROUP_COUNT; a larger value is capped to it.
    size_t max_groups = 0;
};

// Exactly one of these is set, decided by the aggregation's function: counts are exact integers (so a
// large count cannot lose precision to a double), everything else is a number that may be absent when
// no row in the group contributed one.
struct AggregateCell {
    std::optional<int64_t> count; // Count, CountDistinct
    std::optional<double> number; // Sum, Min, Max, Avg -- absent when nothing contributed
};

struct AggregateGroup {
    // Parallel to AggregateQuery::group_by, and empty for a global aggregate.
    std::vector<Value> key;

    // Rows that fell in this group, regardless of whether any aggregation could read a value from
    // them -- so a group of 10 rows where only 3 had numeric objects reports row_count 10 and an
    // average over 3, and the caller can tell the difference.
    int64_t row_count = 0;

    // Parallel to AggregateQuery::aggregations.
    std::vector<AggregateCell> values;
};

struct AggregateResult {
    // Ordered deterministically by key (kind first, then the kind's value), so repeated runs and
    // index-selected versus scanned evaluation return groups in the same order.
    std::vector<AggregateGroup> groups;
};

} // namespace knk
