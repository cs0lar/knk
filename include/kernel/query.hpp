#pragma once

// The query IR: a closed, typed, serializable description of a read, and the first piece of Phase 10
// (see AGENTS.md's "Current Roadmap" and "Query Engine" sections). Deliberately NOT a SQL dialect --
// there is no text, no parser and no grammar here, for the same reason KernelCommand is a struct
// rather than a wire language: an MCP client can discover this from a JSON Schema, and the kernel
// never owns a parser it would then have to keep compatible.
//
// Phase 10's scope is the shape plus parity with the existing query methods. Predicates over object
// values, projection, aggregation and planning all arrive in later phases; every field here is a
// filter the existing methods already apply somewhere, just decomposed so they can be combined.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "kernel/assertion.hpp"
#include "kernel/ids.hpp"
#include "kernel/status.hpp"
#include "kernel/time.hpp"

namespace knk {

// Bumped whenever the meaning of an existing field changes or a field is removed -- never for a purely
// additive field with a backward-compatible default. A Query arriving with an ir_version this build
// does not know is rejected rather than guessed at, so a stored or forwarded query can never be
// silently reinterpreted.
//
// Scheduled for Phase 18 originally; pulled forward into Phase 10 because the MCP `query` tool is
// callable the moment it ships, so the first stored query predates any later versioning retrofit.
constexpr uint32_t QUERY_IR_VERSION = 1;

// Hard ceiling on rows one query may return, independent of the caller's limit. The engine is
// reachable over MCP, where an unbounded result is a denial of service against the host process, so
// there is no "no limit" option: limit == 0 means "as many as the ceiling allows", not "all of them".
// QueryResult::truncated then tells the caller whether more matched.
constexpr size_t MAX_QUERY_RESULT = 10'000;

// Which field orders the result. Every ordering breaks ties on AssertionId, so a Query always has one
// deterministic answer -- notably unlike current()/valid_at()/known_at(), whose order falls out of
// unordered_map iteration and may differ between runs (see docs/query_semantics.md).
enum class QueryOrder : uint8_t { AssertionId, ValidFrom, ObservedAt };

// --- Filters (Phase 11) ----------------------------------------------------------
//
// The selectors on Query are equality on one id; a Filter is everything else: ordered comparisons,
// comparisons against the object's *value* rather than its id, and boolean combinations. The two are
// separate because selectors are what index selection can push down, while a Filter is evaluated per
// row -- keeping that distinction visible in the IR rather than hiding it in a planner.
enum class FilterField : uint8_t {
    Subject,     // operand kind Int64 (an id)
    Predicate,   // operand kind Int64
    Object,      // operand kind Int64
    ObjectValue, // the object's interned Value; operand may be any kind
    Confidence,  // operand kind Double
    ValidFrom,   // operand kind Timestamp
    ValidTo,     // operand kind Timestamp
    ObservedAt,  // operand kind Timestamp
    Status,      // operand kind Text, holding a status name ("Active", "Superseded", ...)
};

enum class CompareOp : uint8_t { Eq, Ne, Lt, Lte, Gt, Gte };

enum class FilterKind : uint8_t { Comparison, And, Or, Not };

// Bound on how deeply filters may nest. A caller-supplied tree is caller-supplied input, so it gets a
// limit for the same reason the result count does; exceeding it is rejected rather than truncated.
constexpr size_t MAX_FILTER_DEPTH = 8;

struct Filter {
    FilterKind kind = FilterKind::Comparison;

    // Meaningful when kind == Comparison.
    FilterField field = FilterField::Confidence;
    CompareOp op = CompareOp::Eq;
    Value operand;

    // Meaningful for And/Or (one or more) and Not (exactly one).
    std::vector<Filter> children;

    static Filter compare(FilterField field, CompareOp op, Value operand) {
        Filter filter;
        filter.kind = FilterKind::Comparison;
        filter.field = field;
        filter.op = op;
        filter.operand = std::move(operand);
        return filter;
    }

    static Filter all_of(std::vector<Filter> children) {
        Filter filter;
        filter.kind = FilterKind::And;
        filter.children = std::move(children);
        return filter;
    }

    static Filter any_of(std::vector<Filter> children) {
        Filter filter;
        filter.kind = FilterKind::Or;
        filter.children = std::move(children);
        return filter;
    }

    static Filter negate(Filter child) {
        Filter filter;
        filter.kind = FilterKind::Not;
        filter.children.push_back(std::move(child));
        return filter;
    }
};

// The catalog lookups a row needed, returned alongside it when Query::resolve_names is set -- so a
// caller rendering query results stops needing a second round trip through the batch resolvers. Only
// the returned page is resolved, never the whole match set.
struct ResolvedNames {
    std::optional<std::string> subject_name;   // absent when the subject is not a text entity
    std::optional<std::string> predicate_name; // absent when the predicate was never interned
    std::optional<Value> object_value;         // the object's interned Value, of whatever kind
};

struct Query {
    uint32_t ir_version = QUERY_IR_VERSION;

    // Absent means "any". subject and object are resolved through merge redirects before matching,
    // exactly as the existing query methods resolve their arguments; predicate is not, because
    // predicates are never merged.
    std::optional<EntityId> subject;
    std::optional<PredicateId> predicate;
    std::optional<EntityId> object;

    // Valid-time point: valid_from <= valid_at < valid_to, with valid_to == OPEN_ENDED meaning no end.
    // Same interval arithmetic (inclusive start, exclusive end) as valid_at().
    std::optional<Timestamp> valid_at;

    // Observed-time window, both bounds inclusive. observed_to mirrors known_at's cutoff; observed_from
    // mirrors changes_since's. Having both directions is what lets one IR express both methods.
    std::optional<Timestamp> observed_from;
    std::optional<Timestamp> observed_to;

    // valid_to == OPEN_ENDED. Combined with statuses == {Active} this is precisely what the
    // current-state index means by "current" (see is_current_assertion), which is how the current*
    // family is expressed in the IR.
    bool open_ended_only = false;

    // Empty means every status, which is what the audit-shaped reads (assertions_for_subject,
    // changes_since) do. Listed explicitly rather than defaulted to Active, so a query never quietly
    // excludes supersessions and retractions the caller wanted.
    std::vector<AssertionStatus> statuses;

    // Everything the selectors above cannot say: ordered comparisons, comparisons against the object's
    // interned Value rather than its id, and boolean combinations. Absent means no filtering. A filter
    // is evaluated per row and never changes which index is selected -- see QueryEngine.
    std::optional<Filter> filter;

    // Resolve the catalog names/values for each returned row (QueryResult::names). Off by default
    // because it costs a lookup per returned row; on, it saves the caller a second round trip through
    // entity_name_batch/predicate_name_batch/entity_value_batch.
    bool resolve_names = false;

    QueryOrder order = QueryOrder::AssertionId;
    bool newest_first = false;

    // limit == 0 means MAX_QUERY_RESULT; any larger value is capped to it. offset is applied after
    // ordering, so paging is stable for a given kernel state.
    size_t limit = 0;
    size_t offset = 0;

    // Keyset pagination (Phase 18): resume strictly after the row this cursor names, in the query's own
    // ordering. Obtained from a previous QueryResult::next_cursor and opaque to the caller.
    //
    // Preferred over offset for walking a large result, for two reasons.
    //
    // The first is depth. A cursor page does *not* skip the selection pass -- every candidate row is
    // filtered on every page either way -- but offset has to order the first offset+limit rows to know
    // which ones its page contains, and that prefix grows as the walk goes deeper, while a cursor's stays
    // the page size. Measured: walking 10,000 rows in pages of 100 costs 19.7 ms by offset and 4.0 ms by
    // cursor, and the deepest offset page alone costs 117 us against 21 us for the first
    // (docs/benchmarks.md).
    //
    // The second is that offset silently shifts when rows are committed underneath it -- page two can
    // repeat or skip rows -- while a cursor names a position in the ordering rather than a count.
    //
    // A cursor and a non-zero offset together are rejected: they are two answers to the same question, and
    // guessing which one a caller meant is worse than saying so. A cursor whose ordering disagrees with
    // the query's is rejected for the same reason.
    std::string cursor;

    // Ceiling on rows the engine may examine before giving up, as a structured QueryBudgetExceeded rather
    // than a slow answer. 0 means no budget, which is what every query written before this field existed
    // carries. The check costs nothing measurable (docs/benchmarks.md), so it is not worth a fast path.
    //
    // Rows examined rather than wall-clock time, deliberately: a time budget makes the same query against
    // the same data succeed or fail depending on machine load, which would undo the reproducibility the
    // rest of the engine works for (a plan that explains itself, results independent of how they were
    // reached). Rows examined is a deterministic proxy for the same protection -- the same query on the
    // same corpus always gets the same answer, budget included.
    size_t max_rows_examined = 0;

    // Diagnostic only, and deliberately not exposed as an MCP tool argument: skip index selection and
    // evaluate against every assertion. Its purpose is differential testing -- every query can be run
    // both ways and the results compared, which is how index use is kept from changing results rather
    // than only cost. Phase 11 builds its randomized differential tests on this.
    bool force_scan = false;

    // Also diagnostic, and also absent from the MCP schema: evaluate against the row layout rather than
    // the columnar store (Phase 15). Index selection and vectorized scanning are two independent choices
    // about *how* a query runs, and both have to be provably irrelevant to *what* it returns -- so each
    // gets a switch, and the differential tests run every generated query through all the combinations.
    bool force_row_scan = false;
};

// Thrown when a query exceeds a budget it was given. A distinct type so a library caller can catch this
// specifically -- "the query was too big" is a different situation from "the query was malformed", and a
// caller may want to retry the first with a narrower filter.
struct QueryBudgetExceeded : std::runtime_error {
    QueryBudgetExceeded(std::string budget_name, size_t limit_value, size_t reached_value)
        : std::runtime_error("query budget '" + budget_name + "' exceeded: limit " + std::to_string(limit_value) +
                             ", reached " + std::to_string(reached_value)),
          budget(std::move(budget_name)), limit(limit_value), reached(reached_value) {}

    std::string budget;
    size_t limit = 0;
    size_t reached = 0;
};

struct QueryResult {
    std::vector<Assertion> assertions;

    // Empty unless Query::resolve_names was set; otherwise parallel to assertions, one entry per
    // returned row. Kept alongside the rows rather than folded into Assertion so the on-disk record
    // shape and the wire shape of an unresolved query are both unchanged.
    std::vector<ResolvedNames> names;

    // Set when more rows remain after this page: pass it back as Query::cursor to continue. Empty when the
    // page is the end of the result, so a caller pages until it is empty rather than counting.
    std::string next_cursor;

    // True when more rows matched than were returned, i.e. the limit (or MAX_QUERY_RESULT) cut the
    // answer short. Distinguishes "exactly this many matched" from "here are the first N", which a
    // bare vector cannot express.
    bool truncated = false;
};

} // namespace knk
