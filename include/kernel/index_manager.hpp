#pragma once

#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "kernel/assertion.hpp"
#include "kernel/ids.hpp"
#include "kernel/subject_predicate_key.hpp"
#include "kernel/time.hpp"

namespace knk {

class IndexManager {
  public:
    void add(const Assertion &assertion);
    void mark_superseded(AssertionId id);
    void mark_retracted(AssertionId id);
    void remove_from_current(AssertionId id);

    std::vector<AssertionId> assertions_for_subject(EntityId subject) const;
    std::vector<PredicateId> predicates_for_subject(EntityId subject) const;
    std::vector<AssertionId> current_assertions(EntityId subject, PredicateId predicate) const;

  private:
    std::unordered_map<EntityId, std::vector<AssertionId>> subject_index_;
    std::unordered_map<SubjectPredicateKey, std::vector<AssertionId>> current_index_;
    std::unordered_map<AssertionId, SubjectPredicateKey> assertion_keys_;
    std::unordered_map<EntityId, std::unordered_set<PredicateId>> predicate_index_;
};

} // namespace knk