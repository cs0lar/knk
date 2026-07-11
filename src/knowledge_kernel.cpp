
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

    bool observed_time_index_restored = false;
    try {
        auto entries = storage_.load_observed_time_index();
        for (const auto &entry : entries) {
            index_manager_.restore_observed_time_entry(entry.subject, entry.observed_at, entry.assertion_id);
        }
        observed_time_index_restored = true;
    } catch (const std::runtime_error &) {
        observed_time_index_restored = false;
    }

    for (const auto &record : log_records) {
        if (observed_time_index_restored) {
            apply_replayed_assertion_without_observed_time(record);
        } else {
            apply_replayed_assertion(record);
        }
    }

    if (!observed_time_index_restored) {
        std::vector<ObservedTimeIndexRecord> records;
        for (const auto &[subject, observed_at, id] : index_manager_.observed_time_entries()) {
            records.push_back(ObservedTimeIndexRecord{subject, observed_at, id});
        }
        storage_.rewrite_observed_time_index(records);
    }
}

void KnowledgeKernel::apply_replayed_assertion(const Assertion &assertion) {
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

void KnowledgeKernel::apply_replayed_assertion_without_observed_time(const Assertion &assertion) {
    if (assertion.supersedes_id != 0) {
        mark_superseded(assertion.supersedes_id);
    }

    if (assertion.retracts_id != 0) {
        mark_retracted(assertion.retracts_id);
    }

    assertions_.push_back(assertion);
    index_manager_.add_without_observed_time(assertion);

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
    apply_replayed_assertion(assertion);

    return id;
}

AssertionId KnowledgeKernel::commit_retraction(EntityId subject, PredicateId predicate, EntityId object,
                                               Timestamp valid_from, Timestamp valid_to, Timestamp observed_at,
                                               double confidence, AssertionId retracts_id) {
    if (retracts_id == 0 || !get(retracts_id).has_value()) {
        throw std::runtime_error("invalid retraction target");
    }

    AssertionId id = next_id_;

    Assertion assertion{
        id, subject,    predicate, object, valid_from, valid_to, observed_at, confidence, AssertionStatus::Retraction,
        0,  retracts_id};

    storage_.append_assertion(assertion);
    storage_.append_observed_time_entry(assertion.subject, assertion.observed_at, assertion.id);
    apply_replayed_assertion(assertion);

    return id;
}

AssertionId KnowledgeKernel::commit_superseding(EntityId subject, PredicateId predicate, EntityId object,
                                                Timestamp valid_from, Timestamp valid_to, Timestamp observed_at,
                                                double confidence, AssertionId supersedes_id) {
    if (supersedes_id == 0 || !get(supersedes_id).has_value()) {
        throw std::runtime_error("invalid superseding target");
    }

    AssertionId id = next_id_;

    Assertion assertion{id,           subject,    predicate,
                        object,       valid_from, valid_to,
                        observed_at,  confidence, AssertionStatus::Active,
                        supersedes_id};

    storage_.append_assertion(assertion);
    storage_.append_observed_time_entry(assertion.subject, assertion.observed_at, assertion.id);
    apply_replayed_assertion(assertion);

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