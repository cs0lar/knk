#include "kernel/ids.hpp"
#include "kernel/subject_predicate_key.hpp"
#include <kernel/index_manager.hpp>
#include <vector>

namespace knk {

void IndexManager::add(const Assertion &assertion) {
    subject_index_[assertion.subject].push_back(assertion.id);
    SubjectPredicateKey key{assertion.subject, assertion.predicate};

    assertion_keys_[assertion.id] = key;

    if (assertion.status == AssertionStatus::Active) {
        current_index_[key].push_back(assertion.id);
    }
}

void IndexManager::mark_superseded(AssertionId id) { remove_from_current(id); }

void IndexManager::mark_retracted(AssertionId id) { remove_from_current(id); }

void IndexManager::remove_from_current(AssertionId id) {
    auto key = assertion_keys_[id];
    auto it = current_index_.find(key);

    if (it != current_index_.end()) {
        current_index_.erase((it));
        assertion_keys_.erase(id);
    }
}

std::vector<AssertionId> IndexManager::assertions_for_subject(EntityId subject) const {

    std::vector<AssertionId> result;

    auto it = subject_index_.find(subject);
    if (it == subject_index_.end()) {
        return result;
    }

    for (AssertionId id : it->second) {
        result.push_back(id);
    }

    return result;
}

std::vector<AssertionId> IndexManager::current_assertions(EntityId subject, PredicateId predicate) const {
    std::vector<AssertionId> result;

    auto key = SubjectPredicateKey{subject = subject, predicate = predicate};

    auto assertion_ids = current_index_.find(key);

    if (assertion_ids == current_index_.end()) {
        return result;
    }

    for (AssertionId id : assertion_ids->second) {
        result.push_back(id);
    }

    return result;
}
} // namespace knk