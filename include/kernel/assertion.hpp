#pragma once

#include <string>

#include "kernel/ids.hpp"
#include "kernel/status.hpp"
#include "kernel/time.hpp"
#include "kernel/value.hpp"

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

// PendingAssertion's name-based counterpart, as handed to KnowledgeKernel::commit_batch_by_name --
// the same relationship commit_by_name has to commit. Subject and predicate are names and the object
// is a Value (a text Value names an entity, any other kind is a literal), exactly as in
// commit_by_name's signature; the kernel interns all three and hands the resulting ids to
// commit_batch.
struct PendingNamedAssertion {
    std::string subject_name;
    std::string predicate_name;
    Value object;

    Timestamp valid_from;
    Timestamp valid_to;
    Timestamp observed_at;

    double confidence;
};

} // namespace knk
