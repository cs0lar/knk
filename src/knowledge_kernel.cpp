
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <vector>

#include "kernel/ids.hpp"
#include "kernel/index_manager.hpp"
#include "kernel/knowledge_kernel.hpp"
#include "kernel/status.hpp"

namespace knk {

KnowledgeKernel::KnowledgeKernel(StorageConfig config) : storage_(config), index_manager_() {
    auto log_records = storage_.load_assertions();

    bool indexes_restored = true;

    try {
        auto entries = storage_.load_observed_time_index();
        for (const auto &entry : entries) {
            index_manager_.restore_observed_time_entry(entry.subject, entry.observed_at, entry.assertion_id);
        }
    } catch (const std::runtime_error &) {
        indexes_restored = false;
    }

    try {
        auto entries = storage_.load_subject_index();
        for (const auto &entry : entries) {
            index_manager_.restore_subject_entry(entry.subject, entry.assertion_id);
        }
    } catch (const std::runtime_error &) {
        indexes_restored = false;
    }

    try {
        auto entries = storage_.load_current_index();
        for (const auto &entry : entries) {
            index_manager_.restore_current_index_entry(entry.subject, entry.predicate, entry.assertion_id,
                                                        entry.active);
        }
    } catch (const std::runtime_error &) {
        indexes_restored = false;
    }

    // A persisted index file is either fully trustworthy or not: if any one of them is missing/corrupt, discard
    // whatever partial state the others contributed and rebuild every index uniformly from the log below, rather
    // than tracking which of the three succeeded independently.
    if (!indexes_restored) {
        index_manager_ = IndexManager();
    }

    for (const auto &record : log_records) {
        if (indexes_restored) {
            restore_assertion(record);
        } else {
            apply(record);
        }
    }

    if (!indexes_restored) {
        std::vector<ObservedTimeIndexRecord> observed_time_records;
        for (const auto &[subject, observed_at, id] : index_manager_.observed_time_entries()) {
            observed_time_records.push_back(ObservedTimeIndexRecord{subject, observed_at, id});
        }
        storage_.rewrite_observed_time_index(observed_time_records);

        std::vector<SubjectIndexRecord> subject_records;
        for (const auto &[subject, id] : index_manager_.subject_index_entries()) {
            subject_records.push_back(SubjectIndexRecord{subject, id});
        }
        storage_.rewrite_subject_index(subject_records);

        std::vector<CurrentIndexRecord> current_records;
        for (const auto &[subject, predicate, id] : index_manager_.current_index_entries()) {
            current_records.push_back(CurrentIndexRecord{subject, predicate, id, true});
        }
        storage_.rewrite_current_index(current_records);
    }
}

void KnowledgeKernel::apply(const Assertion &assertion) {
    if (assertion.supersedes_id != 0) {
        mark_superseded(assertion.supersedes_id);
    }

    if (assertion.retracts_id != 0) {
        mark_retracted(assertion.retracts_id);
    }

    assertions_.push_back(assertion);
    index_manager_.add(assertion);

    next_id_ = std::max(next_id_, assertion.id + 1);
}

void KnowledgeKernel::restore_assertion(const Assertion &assertion) {
    if (assertion.supersedes_id != 0) {
        assertions_[assertion.supersedes_id - 1].status = AssertionStatus::Superseded;
    }

    if (assertion.retracts_id != 0) {
        assertions_[assertion.retracts_id - 1].status = AssertionStatus::Retracted;
    }

    assertions_.push_back(assertion);

    next_id_ = std::max(next_id_, assertion.id + 1);
}

void KnowledgeKernel::mark_superseded(AssertionId superseded_id) {
    assertions_[superseded_id - 1].status = AssertionStatus::Superseded;
    index_manager_.mark_superseded(superseded_id);
}

void KnowledgeKernel::mark_retracted(AssertionId retracted_id) {
    assertions_[retracted_id - 1].status = AssertionStatus::Retracted;
    index_manager_.mark_retracted(retracted_id);
}

AssertionId KnowledgeKernel::commit(EntityId subject, PredicateId predicate, EntityId object, Timestamp valid_from,
                                    Timestamp valid_to, Timestamp observed_at, double confidence) {
    AssertionId id = next_id_;

    Assertion assertion{
        id, subject, predicate, object, valid_from, valid_to, observed_at, confidence, AssertionStatus::Active};

    storage_.append_assertion(assertion);
    storage_.append_observed_time_entry(assertion.subject, assertion.observed_at, assertion.id);
    storage_.append_subject_entry(assertion.subject, assertion.id);
    storage_.append_current_index_entry(assertion.subject, assertion.predicate, assertion.id,
                                        is_current_assertion(assertion));
    apply(assertion);

    return id;
}

AssertionId KnowledgeKernel::commit_retraction(EntityId subject, PredicateId predicate, EntityId object,
                                               Timestamp valid_from, Timestamp valid_to, Timestamp observed_at,
                                               double confidence, AssertionId retracts_id) {
    auto target = get(retracts_id);
    if (retracts_id == 0 || !target.has_value()) {
        throw std::runtime_error("invalid retraction target");
    }

    AssertionId id = next_id_;

    Assertion assertion{
        id, subject,    predicate, object, valid_from, valid_to, observed_at, confidence, AssertionStatus::Retraction,
        0,  retracts_id};

    storage_.append_assertion(assertion);
    storage_.append_observed_time_entry(assertion.subject, assertion.observed_at, assertion.id);
    storage_.append_subject_entry(assertion.subject, assertion.id);
    // Retraction-status records are never current (see is_current_assertion), so only the retracted
    // target needs a current-index tombstone here.
    storage_.append_current_index_entry(target->subject, target->predicate, retracts_id, false);
    apply(assertion);

    return id;
}

AssertionId KnowledgeKernel::commit_superseding(EntityId subject, PredicateId predicate, EntityId object,
                                                Timestamp valid_from, Timestamp valid_to, Timestamp observed_at,
                                                double confidence, AssertionId supersedes_id) {
    auto target = get(supersedes_id);
    if (supersedes_id == 0 || !target.has_value()) {
        throw std::runtime_error("invalid superseding target");
    }

    AssertionId id = next_id_;

    Assertion assertion{id,           subject,    predicate,
                        object,       valid_from, valid_to,
                        observed_at,  confidence, AssertionStatus::Active,
                        supersedes_id};

    storage_.append_assertion(assertion);
    storage_.append_observed_time_entry(assertion.subject, assertion.observed_at, assertion.id);
    storage_.append_subject_entry(assertion.subject, assertion.id);
    storage_.append_current_index_entry(assertion.subject, assertion.predicate, assertion.id,
                                        is_current_assertion(assertion));
    storage_.append_current_index_entry(target->subject, target->predicate, supersedes_id, false);
    apply(assertion);

    return id;
}

std::optional<Assertion> KnowledgeKernel::get(AssertionId id) const {
    if (id == 0 || id >= next_id_) {
        return std::nullopt;
    }

    return assertions_[id - 1];
}

std::vector<Assertion> KnowledgeKernel::assertions_for_subject(EntityId subject) const {
    std::vector<Assertion> result;

    auto assertions = index_manager_.assertions_for_subject(subject);
    if (assertions.empty()) {
        return result;
    }

    for (AssertionId id : assertions) {
        auto assertion = get(id);

        if (assertion.has_value()) {
            result.push_back(*assertion);
        }
    }

    return result;
}

std::vector<Assertion> KnowledgeKernel::current(EntityId subject) const {
    std::vector<Assertion> result;

    auto predicates = index_manager_.predicates_for_subject(subject);

    for (PredicateId predicate : predicates) {
        auto current_assertions = index_manager_.current_assertions(subject, predicate);

        for (AssertionId id : current_assertions) {
            auto assertion = get(id);
            if (assertion.has_value()) {
                result.push_back(*assertion);
            }
        }
    }

    return result;
}

std::vector<Assertion> KnowledgeKernel::valid_at(EntityId subject, Timestamp t) const {
    std::vector<Assertion> result;

    auto assertions = index_manager_.assertions_for_subject(subject);
    if (assertions.empty()) {
        return result;
    }

    for (AssertionId id : assertions) {
        auto assertion = get(id);

        if (!assertion.has_value()) {
            continue;
        }

        bool starts_before_or_at = assertion->valid_from <= t;
        bool ends_after = assertion->valid_to == OPEN_ENDED || t < assertion->valid_to;

        if (assertion->status == AssertionStatus::Active && starts_before_or_at && ends_after) {
            result.push_back(*assertion);
        }
    }

    return result;
}

std::vector<Assertion> KnowledgeKernel::known_at(EntityId subject, Timestamp t) const {
    std::vector<Assertion> result;

    auto assertions = index_manager_.observed_before(subject, t);
    if (assertions.empty()) {
        return result;
    }

    for (AssertionId id : assertions) {
        auto assertion = get(id);
        if (!assertion.has_value()) {
            continue;
        }

        if (assertion->status == AssertionStatus::Active) {
            result.push_back(*assertion);
        }
    }

    return result;
}

std::vector<Assertion> KnowledgeKernel::valid_at_known_at(EntityId subject, Timestamp valid_time,
                                                          Timestamp observed_time) const {
    std::vector<Assertion> result;

    auto assertions = index_manager_.observed_before(subject, observed_time);
    if (assertions.empty()) {
        return result;
    }

    for (AssertionId id : assertions) {
        auto assertion = get(id);
        if (!assertion.has_value()) {
            continue;
        }

        bool starts_before_or_at = assertion->valid_from <= valid_time;
        bool ends_after = assertion->valid_to == OPEN_ENDED || valid_time < assertion->valid_to;

        if (assertion->status == AssertionStatus::Active && starts_before_or_at && ends_after) {
            result.push_back(*assertion);
        }
    }

    return result;
}

std::vector<Assertion> KnowledgeKernel::valid_time_timeline(EntityId subject, PredicateId predicate) const {
    std::vector<Assertion> result;

    auto assertions = index_manager_.assertions_for_subject(subject);
    if (assertions.empty()) {
        return result;
    }

    for (AssertionId id : assertions) {
        auto assertion = get(id);
        if (!assertion.has_value()) {
            continue;
        }

        if (assertion->status == AssertionStatus::Active && assertion->predicate == predicate) {
            result.push_back(*assertion);
        }
    }

    std::sort(result.begin(), result.end(),
              [](const Assertion &a, const Assertion &b) { return a.valid_from < b.valid_from; });

    return result;
}

std::vector<Assertion> KnowledgeKernel::observed_time_timeline(EntityId subject, PredicateId predicate) const {
    std::vector<Assertion> result;

    auto assertions = index_manager_.assertions_for_subject(subject);
    if (assertions.empty()) {
        return result;
    }

    for (AssertionId id : assertions) {
        auto assertion = get(id);
        if (!assertion.has_value()) {
            continue;
        }

        if (assertion->status == AssertionStatus::Active && assertion->predicate == predicate) {
            result.push_back(*assertion);
        }
    }

    std::sort(result.begin(), result.end(),
              [](const Assertion &a, const Assertion &b) { return a.observed_at < b.observed_at; });

    return result;
}

std::vector<Assertion> KnowledgeKernel::commit_history(EntityId subject, PredicateId predicate) const {
    std::vector<Assertion> result;

    auto assertions = index_manager_.assertions_for_subject(subject);
    if (assertions.empty()) {
        return result;
    }

    for (AssertionId id : assertions) {
        auto assertion = get(id);
        if (!assertion.has_value()) {
            continue;
        }

        if (assertion->predicate == predicate) {
            result.push_back(*assertion);
        }
    }

    std::sort(result.begin(), result.end(), [](const Assertion &a, const Assertion &b) { return a.id < b.id; });

    return result;
}

} // namespace knk