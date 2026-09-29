#include <algorithm>
#include <utility>
#include <vector>

#include "kernel/assertion.hpp"
#include "kernel/ids.hpp"
#include "kernel/index_manager.hpp"
#include "kernel/subject_predicate_key.hpp"
#include "kernel/time.hpp"

namespace knk {

bool is_current_assertion(const Assertion &assertion) {
    return assertion.status == AssertionStatus::Active && assertion.valid_to == OPEN_ENDED;
}

void IndexManager::add(const Assertion &assertion) {
    restore_current_index_entry(assertion.subject, assertion.predicate, assertion.id, is_current_assertion(assertion));
    restore_object_entry(assertion.object, assertion.id, is_current_assertion(assertion));
    restore_subject_entry(assertion.subject, assertion.id);
    restore_observed_time_entry(assertion.subject, assertion.observed_at, assertion.id);
}

void IndexManager::restore_current_index_entry(EntityId subject, PredicateId predicate, AssertionId id, bool active) {
    if (!active) {
        remove_from_current(id);
        return;
    }

    SubjectPredicateKey key{subject, predicate};
    current_index_[key].push_back(id);
    assertion_keys_[id] = key;
    predicate_index_[subject].insert(predicate);
    predicate_current_index_[predicate].push_back(id);
}

void IndexManager::restore_object_entry(EntityId object, AssertionId id, bool active) {
    if (!active) {
        remove_from_object_index(id);
        return;
    }

    object_index_[object].push_back(id);
    assertion_object_[id] = object;
}

void IndexManager::restore_observed_time_entry(EntityId subject, Timestamp observed_at, AssertionId id) {
    auto &observed_entries = observed_time_index_[subject];
    auto insert_at = std::upper_bound(observed_entries.begin(), observed_entries.end(), std::make_pair(observed_at, id),
                                      [](const auto &lhs, const auto &rhs) { return lhs.first < rhs.first; });
    observed_entries.insert(insert_at, {observed_at, id});
}

void IndexManager::restore_subject_entry(EntityId subject, AssertionId id) {
    auto &subject_entries = subject_index_[subject];
    subject_entries.push_back(id);
}

void IndexManager::mark_superseded(AssertionId id) {
    remove_from_current(id);
    remove_from_object_index(id);
}

void IndexManager::mark_retracted(AssertionId id) {
    remove_from_current(id);
    remove_from_object_index(id);
}

void IndexManager::remove_from_current(AssertionId id) {

    auto key = assertion_keys_.find(id);

    if (key != assertion_keys_.end()) {
        auto mapIt = current_index_.find(key->second);

        if (mapIt != current_index_.end()) {
            std::erase(mapIt->second, id);
            if (mapIt->second.empty()) {
                predicate_index_[key->second.subject].erase(key->second.predicate);
            }
        }

        auto predicate_entries = predicate_current_index_.find(key->second.predicate);
        if (predicate_entries != predicate_current_index_.end()) {
            std::erase(predicate_entries->second, id);
        }

        assertion_keys_.erase(id);
    }
}

void IndexManager::remove_from_object_index(AssertionId id) {
    auto object = assertion_object_.find(id);

    if (object != assertion_object_.end()) {
        auto entries = object_index_.find(object->second);

        if (entries != object_index_.end()) {
            std::erase(entries->second, id);
        }

        assertion_object_.erase(object);
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

std::vector<AssertionId> IndexManager::current_assertions_by_object(EntityId object) const {
    std::vector<AssertionId> result;

    auto assertion_ids = object_index_.find(object);

    if (assertion_ids == object_index_.end()) {
        return result;
    }

    for (AssertionId id : assertion_ids->second) {
        result.push_back(id);
    }

    return result;
}

std::vector<AssertionId> IndexManager::current_assertions_by_predicate(PredicateId predicate) const {
    std::vector<AssertionId> result;

    auto assertion_ids = predicate_current_index_.find(predicate);

    if (assertion_ids == predicate_current_index_.end()) {
        return result;
    }

    for (AssertionId id : assertion_ids->second) {
        result.push_back(id);
    }

    return result;
}

size_t IndexManager::row_count_for_subject(EntityId subject) const {
    auto it = subject_index_.find(subject);
    return it == subject_index_.end() ? 0 : it->second.size();
}

size_t IndexManager::observed_before_count(EntityId subject, Timestamp t) const {
    auto it = observed_time_index_.find(subject);
    if (it == observed_time_index_.end()) {
        return 0;
    }

    // The same binary search observed_before does, without materializing the ids -- the point of having
    // a count: planning must not cost as much as the work it is choosing between.
    auto end = std::upper_bound(it->second.begin(), it->second.end(), t,
                                [](Timestamp value, const auto &entry) { return value < entry.first; });
    return static_cast<size_t>(std::distance(it->second.begin(), end));
}

size_t IndexManager::current_row_count_by_object(EntityId object) const {
    auto it = object_index_.find(object);
    return it == object_index_.end() ? 0 : it->second.size();
}

size_t IndexManager::current_row_count_by_predicate(PredicateId predicate) const {
    auto it = predicate_current_index_.find(predicate);
    return it == predicate_current_index_.end() ? 0 : it->second.size();
}

size_t IndexManager::distinct_subjects() const { return subject_index_.size(); }

size_t IndexManager::distinct_current_objects() const { return object_index_.size(); }

std::vector<AssertionId> IndexManager::observed_before(EntityId subject, Timestamp t) const {
    std::vector<AssertionId> result;

    auto it = observed_time_index_.find(subject);

    if (it == observed_time_index_.end()) {
        return result;
    }

    auto end = std::upper_bound(it->second.begin(), it->second.end(), t,
                                [](Timestamp value, const auto &entry) { return value < entry.first; });

    for (auto entry = it->second.begin(); entry != end; ++entry) {
        result.push_back(entry->second);
    }

    return result;
}

std::vector<std::tuple<EntityId, Timestamp, AssertionId>> IndexManager::observed_time_entries() const {
    std::vector<std::tuple<EntityId, Timestamp, AssertionId>> result;

    for (const auto &[subject, entries] : observed_time_index_) {
        for (const auto &[observed_at, id] : entries) {
            result.emplace_back(subject, observed_at, id);
        }
    }

    return result;
}

std::vector<std::pair<EntityId, AssertionId>> IndexManager::subject_index_entries() const {
    std::vector<std::pair<EntityId, AssertionId>> result;

    for (const auto &[subject, ids] : subject_index_) {
        for (AssertionId id : ids) {
            result.emplace_back(subject, id);
        }
    }

    return result;
}

std::vector<std::tuple<EntityId, PredicateId, AssertionId>> IndexManager::current_index_entries() const {
    std::vector<std::tuple<EntityId, PredicateId, AssertionId>> result;

    for (const auto &[key, ids] : current_index_) {
        for (AssertionId id : ids) {
            result.emplace_back(key.subject, key.predicate, id);
        }
    }

    return result;
}

} // namespace knk