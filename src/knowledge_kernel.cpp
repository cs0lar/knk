
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
    // Catalog replay is deliberately not part of the snapshot/checkpoint/tail-vs-full-replay
    // branching below: assertions.log never stores names or values, so there is nothing to
    // rebuild this mapping from if entities.log/predicates.log is missing or corrupt. Unlike that
    // branching's try/catch-and-self-heal handling of the derived Phase 3 indexes, a non-tail-
    // corrupt catalog file throws std::runtime_error straight out of this constructor -- a fatal
    // startup error, the same treatment AssertionLog itself gets.
    for (const auto &record : storage_.load_entity_catalog()) {
        catalog_.add_entity(record.id, record.value);
    }

    for (const auto &record : storage_.load_predicate_catalog()) {
        catalog_.add_predicate(record.id, record.name);
    }

    // A snapshot only ever benefits the "indexes trusted" fast path below: the full-rebuild fallback
    // path must apply() every record from id 1 to rebuild IndexManager from scratch regardless, and
    // pre-seeding assertions_ from a snapshot while also apply()-ing those same records would
    // double-push them. So here we only decide whether a usable snapshot exists and, if so, read just
    // the log tail after it; if the fast path turns out not to apply below, the full log is re-read.
    auto snapshot = storage_.load_snapshot();
    bool used_snapshot =
        snapshot.has_value() && snapshot->last_snapshotted_id <= storage_.assertion_log_record_count_hint();

    auto tail_records =
        used_snapshot ? storage_.load_assertions_after(snapshot->last_snapshotted_id) : storage_.load_assertions();

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

    // The persisted indexes are trusted only if every file loaded cleanly AND the checkpoint confirms every
    // assertion in the log had its index writes fully completed -- otherwise a crash could have left the index
    // logs silently behind assertions.log (missing entries for the newest commits, not distinguishable from those
    // entries never existing). Either way, discard whatever partial state was loaded and rebuild every index
    // uniformly from the log below, rather than trying to reconcile per-index-file coverage.
    AssertionId max_committed_id =
        tail_records.empty() ? (used_snapshot ? snapshot->last_snapshotted_id : 0) : tail_records.back().id;
    bool checkpoint_matches = storage_.load_checkpoint() == max_committed_id;

    if (!indexes_restored || !checkpoint_matches) {
        index_manager_ = IndexManager();
        indexes_restored = false;
    }

    if (indexes_restored) {
        if (used_snapshot) {
            assertions_ = snapshot->assertions;
            next_id_ = snapshot->last_snapshotted_id + 1;
        }

        for (const auto &record : tail_records) {
            restore_assertion(record);
        }
    } else {
        // The fallback path always needs the full log to rebuild IndexManager via apply(); if only
        // the tail was read above (because a snapshot looked usable), fetch the rest now.
        auto full_records = used_snapshot ? storage_.load_assertions() : std::move(tail_records);

        for (const auto &record : full_records) {
            apply(record);
        }

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

        storage_.write_checkpoint(max_committed_id);
    }
}

void KnowledgeKernel::write_snapshot() { storage_.write_snapshot(next_id_ - 1, assertions_); }

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
    storage_.write_checkpoint(assertion.id);
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
    storage_.write_checkpoint(assertion.id);
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
    storage_.write_checkpoint(assertion.id);
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

EntityId KnowledgeKernel::intern_entity(std::string_view name) {
    return intern_value(Value::of_text(std::string(name)));
}

EntityId KnowledgeKernel::intern_value(const Value &value) {
    if (auto existing = catalog_.find_entity(value)) {
        return *existing;
    }

    EntityId id = catalog_.next_entity_id();

    storage_.append_entity_catalog_entry(id, value);
    catalog_.add_entity(id, value);

    return id;
}

PredicateId KnowledgeKernel::intern_predicate(std::string_view name) {
    std::string key(name);

    if (auto existing = catalog_.find_predicate(key)) {
        return *existing;
    }

    PredicateId id = catalog_.next_predicate_id();

    storage_.append_predicate_catalog_entry(id, key);
    catalog_.add_predicate(id, key);

    return id;
}

std::optional<EntityId> KnowledgeKernel::find_entity(std::string_view name) const {
    return find_value(Value::of_text(std::string(name)));
}

std::optional<EntityId> KnowledgeKernel::find_value(const Value &value) const { return catalog_.find_entity(value); }

std::optional<PredicateId> KnowledgeKernel::find_predicate(std::string_view name) const {
    return catalog_.find_predicate(std::string(name));
}

std::optional<std::string> KnowledgeKernel::entity_name(EntityId id) const {
    auto value = catalog_.entity_value(id);
    if (!value.has_value() || value->kind != ValueKind::Text) {
        return std::nullopt;
    }

    return value->text;
}

std::optional<Value> KnowledgeKernel::entity_value(EntityId id) const { return catalog_.entity_value(id); }

std::optional<std::string> KnowledgeKernel::predicate_name(PredicateId id) const { return catalog_.predicate_name(id); }

} // namespace knk