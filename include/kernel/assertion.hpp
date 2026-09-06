#pragma once

#include "kernel/ids.hpp"
#include "kernel/status.hpp"
#include "kernel/time.hpp"

namespace knk {

struct Assertion {
    AssertionId id;

    EntityId subject;
    PredicateId predicate;
    EntityId object;

    Timestamp valid_from;
    Timestamp valid_to;
    Timestamp observed_at;

    double confidence;

    AssertionStatus status;

    AssertionId supersedes_id = 0;
    AssertionId retracts_id = 0;
};

// One not-yet-committed assertion, as handed to KnowledgeKernel::commit_batch. Deliberately lacks
// `id` and `status`: the kernel assigns the id (batch entries take consecutive ids in input order)
// and every batch entry is committed Active. It also lacks supersedes_id/retracts_id -- a batch is a
// plain multi-append, never a correction path, so a supersession or retraction stays a single
// commit_superseding/commit_retraction call whose target validation happens before anything is
// appended. Each entry carries its own valid_from/valid_to/observed_at so a batch that restates
// existing records can preserve their per-record valid time rather than stamping the batch's own.
struct PendingAssertion {
    EntityId subject;
    PredicateId predicate;
    EntityId object;

    Timestamp valid_from;
    Timestamp valid_to;
    Timestamp observed_at;

    double confidence;
};

} // namespace knk
