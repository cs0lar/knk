#pragma once

#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "kernel/assertion.hpp"
#include "kernel/ids.hpp"
#include "kernel/subject_predicate_key.hpp"
#include "kernel/time.hpp"

namespace knk {

bool is_current_assertion(const Assertion &assertion);

class IndexManager {
  public:
    void add(const Assertion &assertion);
    void restore_observed_time_entry(EntityId subject, Timestamp observed_at, AssertionId id);
    void restore_subject_entry(EntityId subject, AssertionId id);
    void restore_current_index_entry(EntityId subject, PredicateId predicate, AssertionId id, bool active);
    void mark_superseded(AssertionId id);
    void mark_retracted(AssertionId id);
    void remove_from_current(AssertionId id);

    std::vector<AssertionId> assertions_for_subject(EntityId subject) const;
    std::vector<PredicateId> predicates_for_subject(EntityId subject) const;
    std::vector<AssertionId> current_assertions(EntityId subject, PredicateId predicate) const;
    std::vector<AssertionId> observed_before(EntityId subject, Timestamp t) const;
    std::vector<std::tuple<EntityId, Timestamp, AssertionId>> observed_time_entries() const;
    std::vector<std::pair<EntityId, AssertionId>> subject_index_entries() const;
    std::vector<std::tuple<EntityId, PredicateId, AssertionId>> current_index_entries() const;

  private:
    std::unordered_map<EntityId, std::vector<AssertionId>> subject_index_;
    std::unordered_map<SubjectPredicateKey, std::vector<AssertionId>> current_index_;
    std::unordered_map<AssertionId, SubjectPredicateKey> assertion_keys_;
    std::unordered_map<EntityId, std::unordered_set<PredicateId>> predicate_index_;
    std::unordered_map<EntityId, std::vector<std::pair<Timestamp, AssertionId>>> observed_time_index_;
};

} // namespace knk