#pragma once

#include <optional>
#include <unordered_map>
#include <vector>

#include "kernel/assertion.hpp"
#include "kernel/ids.hpp"
#include "kernel/index_manager.hpp"
#include "kernel/status.hpp"
#include "kernel/storage_engine.hpp"
#include "kernel/time.hpp"

namespace knk {

class KnowledgeKernel {
  public:
    explicit KnowledgeKernel(StorageConfig config);

    AssertionId commit(EntityId subject, PredicateId predicate, EntityId object, Timestamp valid_from,
                       Timestamp valid_to, Timestamp observed_at, double confidence);

    AssertionId commit_retraction(EntityId subject, PredicateId predicate, EntityId object, Timestamp valid_from,
                                  Timestamp valid_to, Timestamp observed_at, double confidence,
                                  AssertionId retracts_id);

    AssertionId commit_superseding(EntityId subject, PredicateId predicate, EntityId object, Timestamp valid_from,
                                   Timestamp valid_to, Timestamp observed_at, double confidence,
                                   AssertionId supersedes_id);

    void apply(const Assertion &assertion);

    void mark_superseded(AssertionId superseded_id);

    void mark_retracted(AssertionId retracted_id);

    std::optional<Assertion> get(AssertionId id) const;

    std::vector<Assertion> assertions_for_subject(EntityId subject) const;

    std::vector<Assertion> current(EntityId subject) const;

    std::vector<Assertion> valid_at(EntityId subject, Timestamp valid_time) const;

    std::vector<Assertion> known_at(EntityId subject, Timestamp observed_time) const;

    std::vector<Assertion> valid_at_known_at(EntityId subject, Timestamp valid_time, Timestamp observed_time) const;

    std::vector<Assertion> valid_time_timeline(EntityId subject, PredicateId predicate) const;

    std::vector<Assertion> observed_time_timeline(EntityId subject, PredicateId predicate) const;

    std::vector<Assertion> commit_history(EntityId subject, PredicateId predicate) const;

  private:
    void restore_assertion(const Assertion &assertion);

    AssertionId next_id_ = 1;

    StorageEngine storage_;

    IndexManager index_manager_;

    std::vector<Assertion> assertions_;
};
} // namespace knk
