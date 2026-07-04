#include "kernel/ids.hpp"
#include "kernel/subject_predicate_key.hpp"
#include "kernel/time.hpp"
#include <kernel/index_manager.hpp>
#include <vector>

namespace knk {

void IndexManager::add(const Assertion &assertion) {
    subject_index_[assertion.subject].push_back(assertion.id);
    SubjectPredicateKey key{assertion.subject, assertion.predicate};

    if (assertion.status == AssertionStatus::Active && assertion.valid_to == OPEN_ENDED) {
        current_index_[key].push_back(assertion.id);
        assertion_keys_[assertion.id] = key;
        predicate_index_[assertion.subject].insert(assertion.predicate);
    }
}

void IndexManager::mark_superseded(AssertionId id) { remove_from_current(id); }

void IndexManager::mark_retracted(AssertionId id) { remove_from_current(id); }

void IndexManager::remove_from_current(AssertionId id) {

    auto key = assertion_keys_.find(id);

    if (key != assertion_keys_.end()) {
        auto mapIt = current_index_.find(key->second);

        if (mapIt != current_index_.end()) {
            std::erase(mapIt->second, id);
            if (mapIt->second.empty()) {
                predicate_index_[key->second.subject].erase(key->second.predicate);
            }
            assertion_keys_.erase(id);
        }
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

std::vector<PredicateId> IndexManager::predicates_for_subject(EntityId subject) const {
    std::vector<PredicateId> result;

    auto it = predicate_index_.find(subject);

    if (it == predicate_index_.end()) {
        return result;
    }

    for (PredicateId id : it->second) {
        result.push_back(id);
    }

    return result;
}

std::vector<AssertionId> IndexManager::current_assertions(EntityId subject, PredicateId predicate) const {
    std::vector<AssertionId> result;

    SubjectPredicateKey key{subject, predicate};

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