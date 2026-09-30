#pragma once

// Schema/corpus discovery (Phase 18; see AGENTS.md's "Query Engine" section).
//
// The query IR is discoverable -- an MCP client reads its JSON Schema and knows every field. What the
// schema cannot say is what is *in* a particular store: which predicates were interned, how far the
// observed-time window reaches, whether a corpus is a thousand rows or ten million. Without that a
// caller either guesses predicate names and gets empty results back, or walks the log to find out.
// These two summaries close that gap, and are the only reason the kernel exposes counts at all.
//
// Both are snapshots of a moment, not live handles: a writer committing concurrently makes them stale
// the instant they are returned, which is fine for their purpose (shaping the next query) and would not
// be for anything that needed them to stay true.

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "kernel/ids.hpp"
#include "kernel/status.hpp"
#include "kernel/time.hpp"

namespace knk {

struct PredicateSummary {
    PredicateId id = 0;
    std::string name;

    // Rows this predicate has in the current-state index: Active and open-ended, i.e. what
    // current_by_predicate would return. Chosen over a total count because it is exact and free (the
    // index already knows), while a total would mean a scan per predicate.
    size_t current_rows = 0;
};

struct CorpusSummary {
    size_t assertion_count = 0;
    size_t entity_count = 0;    // ids allocated, including merged-away ones and document ids
    size_t predicate_count = 0; // interned predicates
    size_t distinct_subjects = 0;
    size_t distinct_current_objects = 0;

    // Every status, always, in enum order -- including the ones with a zero count. A caller rendering
    // this should not have to know which statuses exist to notice that none were retracted.
    std::vector<std::pair<AssertionStatus, size_t>> status_counts;

    // Absent on an empty corpus. valid_to is deliberately not summarized: OPEN_ENDED is 0, so its
    // min/max would say "0" for any store holding a single current fact, which is worse than nothing.
    std::optional<Timestamp> min_observed_at;
    std::optional<Timestamp> max_observed_at;
    std::optional<Timestamp> min_valid_from;
    std::optional<Timestamp> max_valid_from;
};

} // namespace knk
