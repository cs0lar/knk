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

    // Diagnostic only, and deliberately not exposed as an MCP tool argument: skip index selection and
    // evaluate against every assertion. Its purpose is differential testing -- every query can be run
    // both ways and the results compared, which is how index use is kept from changing results rather
    // than only cost. Phase 11 builds its randomized differential tests on this.
    bool force_scan = false;
};

struct QueryResult {
    std::vector<Assertion> assertions;

    // Empty unless Query::resolve_names was set; otherwise parallel to assertions, one entry per
    // returned row. Kept alongside the rows rather than folded into Assertion so the on-disk record
    // shape and the wire shape of an unresolved query are both unchanged.
    std::vector<ResolvedNames> names;

    // True when more rows matched than were returned, i.e. the limit (or MAX_QUERY_RESULT) cut the
    // answer short. Distinguishes "exactly this many matched" from "here are the first N", which a
    // bare vector cannot express.
    bool truncated = false;
};

} // namespace knk
