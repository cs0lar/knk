#include <algorithm>

#include "kernel/query_plan.hpp"
#include "kernel/query_planner.hpp"

namespace knk {

namespace {

// --- the cost model ---------------------------------------------------------------
//
// Units are nanoseconds, and the constants are **measured, not guessed**: `query_benchmark`'s "cost
// model inputs" section times two index lookups differing only in candidate count (which separates the
// fixed lookup from the per-candidate cost) and the two scans. As recorded in docs/benchmarks.md:
//
//     per candidate row, index path     5.15 ns
//     per row, columnar scan            0.91 ns
//     per row, row scan                 1.54 ns
//
// The ratio is what actually decides anything: an index row costs about 5.7x a scanned column row,
// because it is a random access into an 88-byte record while the scan streams one contiguous column. So
// an index wins only while it yields fewer than roughly one row in six of the corpus -- which is the
// rule this model encodes, and the reason a "use the index whenever one applies" heuristic is wrong for
// a common predicate.
//
// These are machine-specific, and deliberately not re-derived at runtime: a planner that calibrates
// itself would make plans unreproducible, which defeats the point of being able to explain one. Re-run
// the benchmark and update them if the shape of the machine changes.
constexpr double INDEX_ROW_COST = 5.15;
constexpr double COLUMN_ROW_COST = 0.91;
constexpr double ROW_SCAN_COST = 1.54;

// The hash lookup and the id vector an index source builds before the executor sees a row. Small, and
// it matters only for the degenerate case of choosing between an index yielding nothing and a scan of a
// nearly empty corpus.
constexpr double INDEX_FIXED_COST = 50.0;

// Evaluating a filter tree costs per surviving row whichever source was chosen, so it cannot change the
// ordering between sources -- it is modelled only so the number in an explanation is honest about what
// the query will actually do.
constexpr double FILTER_ROW_COST = 3.0;

bool is_current_shaped(const Query &query) {
    return query.open_ended_only && query.statuses.size() == 1 && query.statuses.front() == AssertionStatus::Active;
}

double index_cost(size_t rows) { return INDEX_FIXED_COST + static_cast<double>(rows) * INDEX_ROW_COST; }

PlanOption usable(PlanSource source, size_t rows, double cost) {
    PlanOption option;
    option.source = source;
    option.rows = rows;
    option.cost = cost;
    return option;
}

PlanOption rejected(PlanSource source, std::string reason) {
    PlanOption option;
    option.source = source;
    option.rejected_because = std::move(reason);
    return option;
}

} // namespace

const char *plan_source_name(PlanSource source) {
    switch (source) {
    case PlanSource::SubjectIndex:
        return "subject_index";
    case PlanSource::ObservedTimeIndex:
        return "observed_time_index";
    case PlanSource::ObjectCurrentIndex:
        return "object_current_index";
    case PlanSource::PredicateCurrentIndex:
        return "predicate_current_index";
    case PlanSource::ColumnarScan:
        return "columnar_scan";
    case PlanSource::RowScan:
        return "row_scan";
    }

    return "row_scan";
}

QueryPlan plan_query(const Query &query, std::optional<EntityId> subject, std::optional<EntityId> object,
                     const IndexManager &index_manager, bool columns_available, size_t total_rows) {
    QueryPlan plan;
    plan.total_rows = total_rows;
    plan.distinct_subjects = index_manager.distinct_subjects();
    plan.filter_evaluated_per_row = query.filter.has_value();

    // Every index source is only *correct* under some condition -- the current-state indexes hold
    // nothing but Active open-ended rows, so a query wanting any other status would silently lose rows
    // through them. Those conditions are checked first and recorded as rejections, so an explanation
    // says why a source was unavailable rather than leaving a caller to guess.
    const bool current_shaped = is_current_shaped(query);

    if (query.force_scan) {
        plan.considered.push_back(rejected(PlanSource::SubjectIndex, "force_scan"));
    } else if (!subject.has_value()) {
        plan.considered.push_back(rejected(PlanSource::SubjectIndex, "query names no subject"));
    } else {
        size_t rows = index_manager.row_count_for_subject(*subject);
        plan.considered.push_back(usable(PlanSource::SubjectIndex, rows, index_cost(rows)));

        auto observed_bound = observed_upper_bound(query);
        if (observed_bound.has_value()) {
            // A prefix of the subject's rows, so never worse and often much smaller.
            size_t observed_rows = index_manager.observed_before_count(*subject, *observed_bound);
            plan.considered.push_back(usable(PlanSource::ObservedTimeIndex, observed_rows, index_cost(observed_rows)));
        } else {
            plan.considered.push_back(rejected(PlanSource::ObservedTimeIndex, "query has no observed_to bound"));
        }
    }

    for (PlanSource source : {PlanSource::ObjectCurrentIndex, PlanSource::PredicateCurrentIndex}) {
        bool names_it = source == PlanSource::ObjectCurrentIndex ? object.has_value() : query.predicate.has_value();

        if (query.force_scan) {
            plan.considered.push_back(rejected(source, "force_scan"));
        } else if (!names_it) {
            plan.considered.push_back(rejected(source, source == PlanSource::ObjectCurrentIndex
                                                           ? "query names no object"
                                                           : "query names no predicate"));
        } else if (query.as_of_commit.has_value() || query.as_of_observed.has_value()) {
            // The current-state indexes describe what is current *now*. An as-of query asks what was
            // current then, and the two differ by exactly the rows the index has already dropped, so it
            // cannot answer -- not a cost judgement, a correctness one.
            plan.considered.push_back(rejected(source, "index describes current state now; query is as-of"));
        } else if (!current_shaped) {
            plan.considered.push_back(
                rejected(source, "index holds only Active open-ended rows; query is not current-shaped"));
        } else {
            size_t rows = source == PlanSource::ObjectCurrentIndex
                              ? index_manager.current_row_count_by_object(*object)
                              : index_manager.current_row_count_by_predicate(*query.predicate);
            plan.considered.push_back(usable(source, rows, index_cost(rows)));
        }
    }

    if (query.force_row_scan) {
        plan.considered.push_back(rejected(PlanSource::ColumnarScan, "force_row_scan"));
    } else if (!columns_available) {
        plan.considered.push_back(rejected(PlanSource::ColumnarScan, "columnar store unavailable"));
    } else {
        plan.considered.push_back(
            usable(PlanSource::ColumnarScan, total_rows, static_cast<double>(total_rows) * COLUMN_ROW_COST));
    }

    // Always available, and therefore always the fallback: every other source can be unusable.
    plan.considered.push_back(usable(PlanSource::RowScan, total_rows, static_cast<double>(total_rows) * ROW_SCAN_COST));

    const PlanOption *best = nullptr;
    for (const auto &option : plan.considered) {
        if (!option.rejected_because.empty()) {
            continue;
        }
        if (best == nullptr || option.cost < best->cost) {
            best = &option;
        }
    }

    plan.chosen = best->source;
    plan.estimated_rows = best->rows;
    plan.estimated_cost = best->cost;

    if (plan.filter_evaluated_per_row) {
        plan.estimated_cost += static_cast<double>(best->rows) * FILTER_ROW_COST;
    }

    return plan;
}

} // namespace knk
