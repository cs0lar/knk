#pragma once

#include <kernel/ids.hpp>
#include <kernel/status.hpp>
#include <kernel/time.hpp>

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

} // namespace knk
