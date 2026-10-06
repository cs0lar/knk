
#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "kernel/ids.hpp"
#include "kernel/index_manager.hpp"
#include "kernel/knowledge_kernel.hpp"
#include "kernel/status.hpp"

namespace knk {

KnowledgeKernel::KnowledgeKernel(StorageConfig config, OpenMode mode) : storage_(config, mode), index_manager_() {
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

    // Document ids share Catalog's entity id counter but have no entities.log record to replay (see
    // Catalog::allocate_entity_id), so id-space continuity across restarts is restored here instead
    // by scanning PayloadStore directly. Reading each payload (rather than just listing ids) also
    // validates it -- corrupt payload content is a fatal startup error, uncaught like the two catalog
    // logs above, since PayloadStore is likewise authoritative with nothing to rebuild it from.
    for (EntityId id : storage_.existing_payload_ids()) {
        storage_.load_payload(id);
        catalog_.note_allocated_entity_id(id);
    }

    // Provenance replay lives in the same uncaught, authoritative block as the catalog above:
    // assertions.log never encodes provenance, so there is nothing to rebuild it from and a non-tail-
    // corrupt provenance.log is a fatal startup error. The log is append-only, so replaying in commit
    // order and overwriting leaves the last-recorded provenance per assertion winning.
    for (const auto &record : storage_.load_provenance()) {
        provenance_[record.assertion_id] = record;
    }

    // Entity-merge replay lives in the same uncaught, authoritative block: assertions.log never
    // encodes a merge decision, so there is nothing to rebuild entity_merges.log from and a non-tail-
    // corrupt file is a fatal startup error, same as the catalog logs above.
    for (const auto &record : storage_.load_entity_merges()) {
        catalog_.add_merge(record.absorbed, record.surviving);
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

            // A v2 snapshot stores appended statuses (see SnapshotStore), so these are exactly the bytes
            // apply() would have recorded. rebuild_status_state() below turns them back into effective
            // statuses using the links the records carry.
            status_history_.appended.clear();
            status_history_.appended.reserve(assertions_.size());
            for (const auto &assertion : assertions_) {
                status_history_.appended.push_back(static_cast<uint8_t>(assertion.status));
            }
        }

        for (const auto &record : tail_records) {
            restore_assertion(record);
        }

        // Before the object index is rebuilt below, which asks is_current_assertion() of every row: on
        // this path assertions_ carries appended statuses until this call derives the effective ones.
        rebuild_status_state();

        // restore_assertion (unlike apply) never touches IndexManager, so the object index -- which
        // has no persisted log of its own to restore from above -- would otherwise come out empty on
        // this fast path. Rebuilding it here is a single linear pass over already-in-memory
        // assertions_, not a disk re-read, so it doesn't undermine the snapshot/checkpoint
        // optimization this branch exists for. Each assertion's status already reflects any later
        // supersession/retraction applied while building assertions_ above, so is_current_assertion
        // evaluates correctly regardless of iteration order.
        for (const auto &assertion : assertions_) {
            index_manager_.restore_object_entry(assertion.object, assertion.id, is_current_assertion(assertion));
        }
    } else {
        // The fallback path always needs the full log to rebuild IndexManager via apply(); if only
        // the tail was read above (because a snapshot looked usable), fetch the rest now.
        auto full_records = used_snapshot ? storage_.load_assertions() : std::move(tail_records);

        for (const auto &record : full_records) {
            apply(record);
        }

        rebuild_status_state();

        // Healing the files is a write, so a read-only open stops here: the rebuild above already
        // happened in memory, which is what makes its answers correct, and the stale or corrupt files on
        // disk are left exactly as they were for a writer to repair later.
        if (storage_.mode() == OpenMode::ReadWrite) {
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
}

void KnowledgeKernel::rebuild_status_state() {
    // The closing links, in one pass. Built here rather than maintained through every replay path: the
    // snapshot fast path assigns assertions_ wholesale and restore_assertion() bypasses apply(), so a
    // single pass is both cheaper to reason about and impossible to get out of step. Commits maintain all
    // of this incrementally from here on.
    //
    // status_history_.appended is *not* rebuilt here: by this point assertions_ no longer knows it, which
    // is exactly why it is recorded on arrival in apply()/restore_assertion() and seeded from the snapshot.
    status_history_.superseded_by.assign(assertions_.size(), 0);
    status_history_.retracted_by.assign(assertions_.size(), 0);

    for (const auto &assertion : assertions_) {
        if (assertion.supersedes_id != 0 && assertion.supersedes_id <= assertions_.size()) {
            status_history_.superseded_by[assertion.supersedes_id - 1] = assertion.id;
        }
        if (assertion.retracts_id != 0 && assertion.retracts_id <= assertions_.size()) {
            status_history_.retracted_by[assertion.retracts_id - 1] = assertion.id;
        }
    }

    // Effective status, derived from those links rather than trusted from wherever assertions_ came from.
    // A v2 snapshot stores appended statuses, so this is what turns them back into current ones -- and on
    // the full-replay path, where restore_assertion()/apply() already applied them, it is idempotent.
    for (size_t row = 0; row < assertions_.size(); ++row) {
        AssertionStatus status = status_history_.as_of_commit(row, next_id_);
        assertions_[row].status = status;
    }

    effective_status_.clear();
    effective_status_.reserve(assertions_.size());
    for (const auto &assertion : assertions_) {
        effective_status_.push_back(static_cast<uint8_t>(assertion.status));
    }
}

void KnowledgeKernel::write_snapshot() {
    require_writable("write_snapshot");
    storage_.write_snapshot(next_id_ - 1, assertions_, status_history_.appended);
}

void KnowledgeKernel::apply(const Assertion &assertion) {
    if (assertion.supersedes_id != 0) {
        mark_superseded(assertion.supersedes_id, assertion.id);
    }

    if (assertion.retracts_id != 0) {
        mark_retracted(assertion.retracts_id, assertion.id);
    }

    assertions_.push_back(assertion);
    effective_status_.push_back(static_cast<uint8_t>(assertion.status));
    status_history_.push(assertion.status);
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

    // Captured here, where it is still the status the record was appended with. A later record may be
    // about to overwrite assertions_[...].status above, and nothing would then remember what this row was
    // committed as -- which is the whole question an as-of query asks.
    status_history_.push(assertion.status);

    next_id_ = std::max(next_id_, assertion.id + 1);
}

void KnowledgeKernel::mark_superseded(AssertionId superseded_id, AssertionId by_id) {
    assertions_[superseded_id - 1].status = AssertionStatus::Superseded;
    if (superseded_id - 1 < effective_status_.size()) {
        // Guarded because replay calls this before the array exists; construction rebuilds it wholesale.
        effective_status_[superseded_id - 1] = static_cast<uint8_t>(AssertionStatus::Superseded);
    }
    if (superseded_id - 1 < status_history_.size()) {
        status_history_.superseded_by[superseded_id - 1] = by_id;
    }
    index_manager_.mark_superseded(superseded_id);
}

void KnowledgeKernel::mark_retracted(AssertionId retracted_id, AssertionId by_id) {
    assertions_[retracted_id - 1].status = AssertionStatus::Retracted;
    if (retracted_id - 1 < effective_status_.size()) {
        effective_status_[retracted_id - 1] = static_cast<uint8_t>(AssertionStatus::Retracted);
    }
    if (retracted_id - 1 < status_history_.size()) {
        status_history_.retracted_by[retracted_id - 1] = by_id;
    }
    index_manager_.mark_retracted(retracted_id);
}

AssertionId KnowledgeKernel::commit(EntityId subject, PredicateId predicate, EntityId object, Timestamp valid_from,
                                    Timestamp valid_to, Timestamp observed_at, double confidence) {
    require_writable("commit");

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

std::vector<AssertionId> KnowledgeKernel::commit_batch(const std::vector<PendingAssertion> &entries) {
    require_writable("commit_batch");

    // Checked before anything is built or appended, so an over-sized batch leaves next_id_ and every
    // log untouched -- the same "a failed call burns no id" property commit_superseding/
    // commit_retraction get from validating their target first.
    if (entries.size() > MAX_BATCH_SIZE) {
        throw std::runtime_error("batch exceeds maximum size");
    }

    if (entries.empty()) {
        return {};
    }

    std::vector<Assertion> assertions;
    assertions.reserve(entries.size());

    std::vector<AssertionId> ids;
    ids.reserve(entries.size());

    std::vector<ObservedTimeIndexRecord> observed_time_records;
    observed_time_records.reserve(entries.size());

    std::vector<SubjectIndexRecord> subject_records;
    subject_records.reserve(entries.size());

    std::vector<CurrentIndexRecord> current_records;
    current_records.reserve(entries.size());

    AssertionId id = next_id_;

    for (const auto &entry : entries) {
        Assertion assertion{id,
                            entry.subject,
                            entry.predicate,
                            entry.object,
                            entry.valid_from,
                            entry.valid_to,
                            entry.observed_at,
                            entry.confidence,
                            AssertionStatus::Active};

        observed_time_records.push_back(
            ObservedTimeIndexRecord{assertion.subject, assertion.observed_at, assertion.id});
        subject_records.push_back(SubjectIndexRecord{assertion.subject, assertion.id});
        current_records.push_back(
            CurrentIndexRecord{assertion.subject, assertion.predicate, assertion.id, is_current_assertion(assertion)});

        assertions.push_back(assertion);
        ids.push_back(id);

        ++id;
    }

    // Durable-before-visible, in the same order a single commit uses -- assertion log, then the three
    // index logs, then the checkpoint -- just once for the whole batch instead of once per assertion.
    storage_.append_assertions(assertions);
    storage_.append_observed_time_entries(observed_time_records);
    storage_.append_subject_entries(subject_records);
    storage_.append_current_index_entries(current_records);
    storage_.write_checkpoint(assertions.back().id);

    for (const auto &assertion : assertions) {
        apply(assertion);
    }

    return ids;
}

std::vector<AssertionId> KnowledgeKernel::commit_batch_by_name(const std::vector<PendingNamedAssertion> &entries) {
    require_writable("commit_batch_by_name");

    // Checked here as well as in commit_batch below, so an over-sized batch is rejected before any of
    // its names are interned rather than after -- a rejected call must leave no trace at all.
    if (entries.size() > MAX_BATCH_SIZE) {
        throw std::runtime_error("batch exceeds maximum size");
    }

    std::vector<PendingAssertion> resolved;
    resolved.reserve(entries.size());

    for (const auto &entry : entries) {
        resolved.push_back(PendingAssertion{intern_entity(entry.subject_name), intern_predicate(entry.predicate_name),
                                            intern_value(entry.object), entry.valid_from, entry.valid_to,
                                            entry.observed_at, entry.confidence});
    }

    return commit_batch(resolved);
}

AssertionId KnowledgeKernel::commit_by_name(std::string_view subject_name, std::string_view predicate_name,
                                            const Value &object, Timestamp valid_from, Timestamp valid_to,
                                            Timestamp observed_at, double confidence) {
    require_writable("commit_by_name");

    EntityId subject = intern_entity(subject_name);
    PredicateId predicate = intern_predicate(predicate_name);
    EntityId object_id = intern_value(object);

    return commit(subject, predicate, object_id, valid_from, valid_to, observed_at, confidence);
}

AssertionId KnowledgeKernel::commit_retraction(EntityId subject, PredicateId predicate, EntityId object,
                                               Timestamp valid_from, Timestamp valid_to, Timestamp observed_at,
                                               double confidence, AssertionId retracts_id) {
    require_writable("commit_retraction");

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
    require_writable("commit_superseding");

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

AssertionId KnowledgeKernel::commit_hypothesis(EntityId subject, PredicateId predicate, EntityId object,
                                               Timestamp valid_from, Timestamp valid_to, Timestamp observed_at,
                                               double confidence, EntityId source, Timestamp recorded_at,
                                               std::string method) {
    require_writable("commit_hypothesis");

    AssertionId id = next_id_;

    Assertion assertion{
        id, subject, predicate, object, valid_from, valid_to, observed_at, confidence, AssertionStatus::Hypothesis};

    storage_.append_assertion(assertion);
    storage_.append_observed_time_entry(assertion.subject, assertion.observed_at, assertion.id);
    storage_.append_subject_entry(assertion.subject, assertion.id);
    // Always false: is_current_assertion requires Active status, so a hypothesis never enters the
    // current-state index. The call still goes through append_current_index_entry uniformly, same as
    // commit()/commit_superseding(), rather than special-casing hypotheses out of that log.
    storage_.append_current_index_entry(assertion.subject, assertion.predicate, assertion.id,
                                        is_current_assertion(assertion));
    storage_.write_checkpoint(assertion.id);
    apply(assertion);

    // An unsourced hypothesis is a contradiction in terms for this design, so provenance is recorded
    // unconditionally rather than left to the caller, reusing record_provenance's own durable-before-
    // visible append instead of duplicating it here.
    record_provenance(id, source, recorded_at, std::move(method));

    return id;
}

std::optional<Assertion> KnowledgeKernel::get(AssertionId id) const {
    if (id == 0 || id >= next_id_) {
        return std::nullopt;
    }

    return assertions_[id - 1];
}

std::vector<Assertion> KnowledgeKernel::assertions_for_subject(EntityId subject, size_t limit) const {
    std::vector<Assertion> result;

    subject = catalog_.resolve(subject);

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

    if (limit > 0 && result.size() > limit) {
        result.resize(limit);
    }

    return result;
}

std::vector<Assertion> KnowledgeKernel::current(EntityId subject) const {
    std::vector<Assertion> result;

    // Resolving here (rather than in IndexManager) is what makes merge_entities take effect at the
    // query boundary only: a caller still holding the absorbed id transparently gets the surviving
    // id's current facts, with no rewrite of assertions_ or the index.
    subject = catalog_.resolve(subject);

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

std::vector<Assertion> KnowledgeKernel::current_by_name(std::string_view subject_name) const {
    auto subject = find_entity(subject_name);
    if (!subject.has_value()) {
        return {};
    }

    return current(*subject);
}

std::vector<Assertion> KnowledgeKernel::current_by_object(EntityId object) const {
    std::vector<Assertion> result;

    object = catalog_.resolve(object);

    for (AssertionId id : index_manager_.current_assertions_by_object(object)) {
        auto assertion = get(id);
        if (assertion.has_value()) {
            result.push_back(*assertion);
        }
    }

    return result;
}

std::vector<Assertion> KnowledgeKernel::current_by_predicate(PredicateId predicate) const {
    std::vector<Assertion> result;

    for (AssertionId id : index_manager_.current_assertions_by_predicate(predicate)) {
        auto assertion = get(id);
        if (assertion.has_value()) {
            result.push_back(*assertion);
        }
    }

    return result;
}

std::vector<Assertion> KnowledgeKernel::hypotheses_for(EntityId subject) const {
    std::vector<Assertion> result;

    subject = catalog_.resolve(subject);

    for (AssertionId id : index_manager_.assertions_for_subject(subject)) {
        auto assertion = get(id);
        if (assertion.has_value() && assertion->status == AssertionStatus::Hypothesis) {
            result.push_back(*assertion);
        }
    }

    return result;
}

std::vector<EntityId> KnowledgeKernel::neighbors(EntityId subject, size_t max_hops) const {
    subject = catalog_.resolve(subject);

    std::vector<EntityId> result;
    std::unordered_set<EntityId> visited{subject};
    std::vector<EntityId> frontier{subject};

    for (size_t hop = 0; hop < max_hops && !frontier.empty(); ++hop) {
        std::vector<EntityId> next_frontier;

        for (EntityId entity : frontier) {
            for (const auto &assertion : current(entity)) {
                if (visited.insert(assertion.object).second) {
                    result.push_back(assertion.object);
                    next_frontier.push_back(assertion.object);
                }
            }

            for (AssertionId id : index_manager_.current_assertions_by_object(entity)) {
                auto assertion = get(id);
                if (assertion.has_value() && visited.insert(assertion->subject).second) {
                    result.push_back(assertion->subject);
                    next_frontier.push_back(assertion->subject);
                }
            }
        }

        frontier = std::move(next_frontier);
    }

    return result;
}

std::vector<PredicateId> KnowledgeKernel::co_occurring_predicates(EntityId subject) const {
    return index_manager_.predicates_for_subject(catalog_.resolve(subject));
}

std::vector<Assertion> KnowledgeKernel::valid_at(EntityId subject, Timestamp t) const {
    std::vector<Assertion> result;

    subject = catalog_.resolve(subject);

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

    subject = catalog_.resolve(subject);

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

    subject = catalog_.resolve(subject);

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

    subject = catalog_.resolve(subject);

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

    subject = catalog_.resolve(subject);

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

std::vector<Assertion> KnowledgeKernel::commit_history(EntityId subject, PredicateId predicate, size_t limit) const {
    std::vector<Assertion> result;

    subject = catalog_.resolve(subject);

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

    if (limit > 0 && result.size() > limit) {
        result.resize(limit);
    }

    return result;
}

std::vector<Assertion> KnowledgeKernel::changes_since(Timestamp observed_since, size_t limit, bool newest_first) const {
    std::vector<Assertion> result;

    for (const auto &assertion : assertions_) {
        if (assertion.observed_at >= observed_since) {
            result.push_back(assertion);
        }
    }

    std::sort(result.begin(), result.end(), [newest_first](const Assertion &a, const Assertion &b) {
        if (a.observed_at != b.observed_at) {
            return newest_first ? a.observed_at > b.observed_at : a.observed_at < b.observed_at;
        }
        return newest_first ? a.id > b.id : a.id < b.id;
    });

    if (limit > 0 && result.size() > limit) {
        result.resize(limit);
    }

    return result;
}

std::vector<Assertion> KnowledgeKernel::explain(AssertionId id) const {
    std::vector<Assertion> chain;

    auto assertion = get(id);
    while (assertion.has_value()) {
        chain.push_back(*assertion);

        // supersedes_id and retracts_id are mutually exclusive on any given record (commit_superseding
        // sets one, commit_retraction the other), so at most one is non-zero. Both always point at an
        // earlier, smaller id, so the chain strictly decreases and terminates -- no cycle guard needed.
        AssertionId next = assertion->supersedes_id != 0 ? assertion->supersedes_id : assertion->retracts_id;
        if (next == 0) {
            break;
        }

        assertion = get(next);
    }

    return chain;
}

std::vector<std::pair<Assertion, Assertion>> KnowledgeKernel::find_conflicts(EntityId subject,
                                                                             PredicateId predicate) const {
    std::vector<std::pair<Assertion, Assertion>> conflicts;

    subject = catalog_.resolve(subject);

    std::vector<Assertion> active;
    for (AssertionId id : index_manager_.assertions_for_subject(subject)) {
        auto assertion = get(id);
        if (assertion.has_value() && assertion->status == AssertionStatus::Active &&
            assertion->predicate == predicate) {
            active.push_back(*assertion);
        }
    }

    // Two half-open [valid_from, valid_to) intervals overlap iff each starts strictly before the
    // other ends; OPEN_ENDED (0) means unbounded, so it never bounds an end.
    auto overlaps = [](const Assertion &a, const Assertion &b) {
        bool a_ends_after_b_starts = a.valid_to == OPEN_ENDED || b.valid_from < a.valid_to;
        bool b_ends_after_a_starts = b.valid_to == OPEN_ENDED || a.valid_from < b.valid_to;
        return a_ends_after_b_starts && b_ends_after_a_starts;
    };

    for (size_t i = 0; i < active.size(); ++i) {
        for (size_t j = i + 1; j < active.size(); ++j) {
            if (active[i].object != active[j].object && overlaps(active[i], active[j])) {
                conflicts.emplace_back(active[i], active[j]);
            }
        }
    }

    return conflicts;
}

EntityId KnowledgeKernel::intern_entity(std::string_view name) {
    require_writable("intern_entity");

    return intern_value(Value::of_text(std::string(name)));
}

EntityId KnowledgeKernel::intern_value(const Value &value) {
    require_writable("intern_value");

    if (auto existing = catalog_.find_entity(value)) {
        return *existing;
    }

    EntityId id = catalog_.next_entity_id();

    storage_.append_entity_catalog_entry(id, value);
    catalog_.add_entity(id, value);

    return id;
}

PredicateId KnowledgeKernel::intern_predicate(std::string_view name) {
    require_writable("intern_predicate");

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

EntityId KnowledgeKernel::intern_document(std::span<const std::byte> content) {
    require_writable("intern_document");

    EntityId id = catalog_.allocate_entity_id();

    storage_.write_payload(id, content);

    return id;
}

std::optional<std::vector<std::byte>> KnowledgeKernel::document_content(EntityId id) const {
    return storage_.load_payload(id);
}

void KnowledgeKernel::record_provenance(AssertionId assertion_id, EntityId source, Timestamp recorded_at,
                                        std::string method) {
    require_writable("record_provenance");

    if (assertion_id == 0 || !get(assertion_id).has_value()) {
        throw std::runtime_error("invalid provenance target");
    }

    // Durable-before-visible: append to the log first, then apply to the in-memory map, exactly like
    // commit's append-then-apply ordering.
    storage_.append_provenance_entry(assertion_id, source, recorded_at, method);
    provenance_[assertion_id] = ProvenanceRecord{assertion_id, source, recorded_at, std::move(method)};
}

void KnowledgeKernel::record_provenance_batch(const std::vector<ProvenanceRecord> &records) {
    require_writable("record_provenance_batch");

    if (records.size() > MAX_BATCH_SIZE) {
        throw std::runtime_error("batch exceeds maximum size");
    }

    if (records.empty()) {
        return;
    }

    // Every target is validated up front, so a bad id anywhere in the list rejects the whole call
    // before a single record is appended -- record_provenance's "a failed call persists nothing"
    // property, extended to the list rather than applied one record at a time.
    for (const auto &record : records) {
        if (record.assertion_id == 0 || !get(record.assertion_id).has_value()) {
            throw std::runtime_error("invalid provenance target");
        }
    }

    // Durable-before-visible, same ordering record_provenance uses, just once for the whole list.
    storage_.append_provenance_entries(records);

    for (const auto &record : records) {
        provenance_[record.assertion_id] = record;
    }
}

std::optional<ProvenanceRecord> KnowledgeKernel::provenance_for(AssertionId assertion_id) const {
    auto it = provenance_.find(assertion_id);
    if (it == provenance_.end()) {
        return std::nullopt;
    }

    return it->second;
}

// Each batch read below is the matching single resolver called once per id, in input order, so every
// slot answers exactly what the single call would -- see the header comment on why an unknown id is a
// nullopt slot here rather than a rejected call.

std::vector<std::optional<std::string>> KnowledgeKernel::entity_name_batch(const std::vector<EntityId> &ids) const {
    if (ids.size() > MAX_BATCH_SIZE) {
        throw std::runtime_error("batch exceeds maximum size");
    }

    std::vector<std::optional<std::string>> names;
    names.reserve(ids.size());

    for (EntityId id : ids) {
        names.push_back(entity_name(id));
    }

    return names;
}

std::vector<std::optional<Value>> KnowledgeKernel::entity_value_batch(const std::vector<EntityId> &ids) const {
    if (ids.size() > MAX_BATCH_SIZE) {
        throw std::runtime_error("batch exceeds maximum size");
    }

    std::vector<std::optional<Value>> values;
    values.reserve(ids.size());

    for (EntityId id : ids) {
        values.push_back(entity_value(id));
    }

    return values;
}

std::vector<std::optional<std::string>>
KnowledgeKernel::predicate_name_batch(const std::vector<PredicateId> &ids) const {
    if (ids.size() > MAX_BATCH_SIZE) {
        throw std::runtime_error("batch exceeds maximum size");
    }

    std::vector<std::optional<std::string>> names;
    names.reserve(ids.size());

    for (PredicateId id : ids) {
        names.push_back(predicate_name(id));
    }

    return names;
}

std::vector<std::optional<ProvenanceRecord>>
KnowledgeKernel::provenance_for_batch(const std::vector<AssertionId> &ids) const {
    if (ids.size() > MAX_BATCH_SIZE) {
        throw std::runtime_error("batch exceeds maximum size");
    }

    std::vector<std::optional<ProvenanceRecord>> records;
    records.reserve(ids.size());

    for (AssertionId id : ids) {
        records.push_back(provenance_for(id));
    }

    return records;
}

// A thin forward to QueryEngine, deliberately: the kernel owns no query logic of its own, so there is
// exactly one place where the bitemporal/status rules compose. The state is passed rather than held,
// so the engine keeps no references into this object.
OpenMode KnowledgeKernel::mode() const { return storage_.mode(); }

void KnowledgeKernel::require_writable(const char *operation) const {
    if (storage_.mode() == OpenMode::ReadOnly) {
        throw std::runtime_error(std::string("kernel is open read-only; ") + operation + " is not allowed");
    }
}

QuerySource KnowledgeKernel::query_source() const {
    // map_columns() caches its mappings, so this is a pointer hand-off after the first call rather than
    // an mmap per query; a commit invalidates it, which is exactly when it should be redone.
    return QuerySource{assertions_,       index_manager_, catalog_, storage_.map_columns(),
                       effective_status_, status_history_};
}

QueryResult KnowledgeKernel::query(const Query &query) const { return query_engine_.execute(query, query_source()); }

SpillDescriptor KnowledgeKernel::spill_query(const Query &query, const std::filesystem::path &directory,
                                             const std::string &token) const {
    // The whole selection, in order: a spill exists to hand over a result too large to page, so the JSON
    // ceiling does not apply here -- MAX_SPILL_ROWS does.
    auto selection = query_engine_.select_rows(query, query_source(), std::numeric_limits<size_t>::max());

    size_t limit = query.limit == 0 ? MAX_SPILL_ROWS : std::min(query.limit, MAX_SPILL_ROWS);
    size_t begin = std::min(query.offset, selection.size());
    size_t end = query.offset > std::numeric_limits<size_t>::max() - limit
                     ? selection.size()
                     : std::min(selection.size(), query.offset + limit);

    std::span<const uint32_t> page(selection.data() + begin, end > begin ? end - begin : 0);

    std::string chosen = token;
    if (chosen.empty()) {
        // Skips what is already there, so a restarted process does not collide with a previous result.
        do {
            chosen = "result-" + std::to_string(next_spill_++);
        } while (std::filesystem::exists(directory / chosen));
    }

    // Under an as-of mode the spilled status column has to be the reconstructed one, for the same reason
    // a returned row's does: the selection was made against it.
    std::vector<uint8_t> as_of_status;
    if (query.as_of_commit.has_value() || query.as_of_observed.has_value()) {
        as_of_status.resize(assertions_.size());
        for (size_t row = 0; row < assertions_.size(); ++row) {
            as_of_status[row] =
                static_cast<uint8_t>(query.as_of_commit.has_value()
                                         ? status_history_.as_of_commit(row, *query.as_of_commit)
                                         : status_history_.as_of_observed(row, *query.as_of_observed, assertions_));
        }
    }

    return write_spill(directory, chosen, page, assertions_, catalog_, as_of_status);
}

QueryPlan KnowledgeKernel::explain_query(const Query &query) const {
    return query_engine_.explain(query, query_source());
}

AggregateResult KnowledgeKernel::aggregate(const AggregateQuery &query) const {
    return query_engine_.aggregate(query, query_source());
}

std::vector<PredicateSummary> KnowledgeKernel::describe_predicates() const {
    std::vector<PredicateSummary> result;

    for (const auto &[id, name] : catalog_.predicates()) {
        PredicateSummary summary;
        summary.id = id;
        summary.name = name;
        summary.current_rows = index_manager_.current_row_count_by_predicate(id);
        result.push_back(std::move(summary));
    }

    return result;
}

CorpusSummary KnowledgeKernel::describe_corpus() const {
    CorpusSummary summary;
    summary.assertion_count = assertions_.size();

    // next_*_id is one past the last allocated, and ids start at 1.
    summary.entity_count = catalog_.next_entity_id() - 1;
    summary.predicate_count = catalog_.predicates().size();
    summary.distinct_subjects = index_manager_.distinct_subjects();
    summary.distinct_current_objects = index_manager_.distinct_current_objects();

    std::array<size_t, 5> counts{};
    for (const auto &assertion : assertions_) {
        size_t slot = static_cast<size_t>(assertion.status);
        if (slot < counts.size()) {
            ++counts[slot];
        }

        if (!summary.min_observed_at.has_value()) {
            summary.min_observed_at = assertion.observed_at;
            summary.max_observed_at = assertion.observed_at;
            summary.min_valid_from = assertion.valid_from;
            summary.max_valid_from = assertion.valid_from;
        } else {
            summary.min_observed_at = std::min(*summary.min_observed_at, assertion.observed_at);
            summary.max_observed_at = std::max(*summary.max_observed_at, assertion.observed_at);
            summary.min_valid_from = std::min(*summary.min_valid_from, assertion.valid_from);
            summary.max_valid_from = std::max(*summary.max_valid_from, assertion.valid_from);
        }
    }

    // Every status listed, zero counts included -- see the note on CorpusSummary::status_counts.
    for (size_t slot = 0; slot < counts.size(); ++slot) {
        summary.status_counts.emplace_back(static_cast<AssertionStatus>(slot), counts[slot]);
    }

    return summary;
}

void KnowledgeKernel::merge_entities(EntityId keep, EntityId absorb, Timestamp merged_at) {
    require_writable("merge_entities");

    // Durable-before-visible: append to the log first, then apply to the in-memory Catalog, exactly
    // like commit's append-then-apply ordering.
    storage_.append_entity_merge_entry(absorb, keep, merged_at);
    catalog_.add_merge(absorb, keep);
}

EntityId KnowledgeKernel::resolve_entity(EntityId id) const { return catalog_.resolve(id); }

void KnowledgeKernel::archive_segments_before(AssertionId assertion_id) {
    require_writable("archive_segments_before");

    storage_.archive_segments_before(assertion_id);
}

} // namespace knk