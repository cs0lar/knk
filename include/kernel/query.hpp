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

    // True when more rows matched than were returned, i.e. the limit (or MAX_QUERY_RESULT) cut the
    // answer short. Distinguishes "exactly this many matched" from "here are the first N", which a
    // bare vector cannot express.
    bool truncated = false;
};

} // namespace knk
