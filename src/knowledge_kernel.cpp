
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

    for (const auto &record : log_records) {
        apply_replayed_assertion(record);
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
    apply_replayed_assertion(assertion);

    return id;
}

AssertionId KnowledgeKernel::commit_superseding(EntityId subject, PredicateId predicate, EntityId object,
                                                Timestamp valid_from, Timestamp valid_to, Timestamp observed_at,
                                                double confidence, AssertionId supersedes_id) {
    AssertionId id = next_id_;

    Assertion assertion{id,           subject,    predicate,
                        object,       valid_from, valid_to,
                        observed_at,  confidence, AssertionStatus::Active,
                        supersedes_id};

    storage_.append_assertion(assertion);
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

    auto assertions = index_manager_.assertions_for_subject(subject);
    if (assertions.empty()) {
        return result;
    }

    for (AssertionId id : assertions) {
        auto assertion = get(id);
        if (!assertion.has_value()) {
            continue;
        }

        bool is_observed = assertion->observed_at <= t;

        if (assertion->status == AssertionStatus::Active && is_observed) {
            result.push_back(*assertion);
        }
    }

    return result;
}

std::vector<Assertion> KnowledgeKernel::valid_at_known_at(EntityId subject, Timestamp valid_time,
                                                          Timestamp observed_time) const {
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

        bool starts_before_or_at = assertion->valid_from <= valid_time;
        bool ends_after = assertion->valid_to == OPEN_ENDED || valid_time < assertion->valid_to;
        bool is_observed = assertion->observed_at <= observed_time;

        if (assertion->status == AssertionStatus::Active && is_observed && starts_before_or_at && ends_after) {
            result.push_back(*assertion);
        }
    }

    return result;
}
} // namespace knk