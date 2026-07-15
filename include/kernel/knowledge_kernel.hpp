#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "kernel/assertion.hpp"
#include "kernel/catalog.hpp"
#include "kernel/ids.hpp"
#include "kernel/index_manager.hpp"
#include "kernel/status.hpp"
#include "kernel/storage_engine.hpp"
#include "kernel/time.hpp"
#include "kernel/value.hpp"

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

    // Persists a full snapshot of the current in-memory assertions_ so a future startup can skip
    // re-parsing the portion of assertions.log it covers. Explicit/caller-triggered only -- there
    // is no automatic cadence, so commit-path latency is unaffected.
    void write_snapshot();

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

    EntityId intern_entity(std::string_view name);

    EntityId intern_value(const Value &value);

    PredicateId intern_predicate(std::string_view name);

    std::optional<EntityId> find_entity(std::string_view name) const;

    std::optional<EntityId> find_value(const Value &value) const;

    std::optional<PredicateId> find_predicate(std::string_view name) const;

    std::optional<std::string> entity_name(EntityId id) const;

    std::optional<Value> entity_value(EntityId id) const;

    std::optional<std::string> predicate_name(PredicateId id) const;

  private:
    void restore_assertion(const Assertion &assertion);

    AssertionId next_id_ = 1;

    StorageEngine storage_;

    IndexManager index_manager_;

    Catalog catalog_;

    std::vector<Assertion> assertions_;
};
} // namespace knk
