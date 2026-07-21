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

    // Reverse (object -> subject) companion to restore_current_index_entry, keyed by object instead
    // of subject/predicate. Deliberately not persisted to its own log: unlike the three Phase 3
    // indexes, it carries no corruption/self-heal/checkpoint machinery of its own -- it is always
    // rebuilt in memory (see KnowledgeKernel's constructor), either via add() during full replay/live
    // commits or via a single bulk pass over already-loaded assertions_ on the fast startup path. Only
    // current (Active, open-ended) assertions are tracked, mirroring restore_current_index_entry, so
    // KnowledgeKernel::neighbors can traverse both edge directions symmetrically.
    void restore_object_entry(EntityId object, AssertionId id, bool active);

    void mark_superseded(AssertionId id);
    void mark_retracted(AssertionId id);
    void remove_from_current(AssertionId id);

    std::vector<AssertionId> assertions_for_subject(EntityId subject) const;
    std::vector<PredicateId> predicates_for_subject(EntityId subject) const;
    std::vector<AssertionId> current_assertions(EntityId subject, PredicateId predicate) const;

    // Current (Active, open-ended) assertion ids for which the given entity is the object -- the
    // reverse-direction counterpart to current_assertions.
    std::vector<AssertionId> current_assertions_by_object(EntityId object) const;

    // Current (Active, open-ended) assertion ids for the given predicate, any subject -- e.g. every
    // WORKS_AT relationship in the kernel. Unlike current_assertions_by_object, this needs no
    // separate restore_*_entry/bulk-seed path: predicate is already part of every persisted
    // current_index.log record, so folding this into restore_current_index_entry (see index_manager.cpp)
    // keeps it correct across restart on both the fast trusted-index path and the full-replay path for
    // free.
    std::vector<AssertionId> current_assertions_by_predicate(PredicateId predicate) const;

    std::vector<AssertionId> observed_before(EntityId subject, Timestamp t) const;
    std::vector<std::tuple<EntityId, Timestamp, AssertionId>> observed_time_entries() const;
    std::vector<std::pair<EntityId, AssertionId>> subject_index_entries() const;
    std::vector<std::tuple<EntityId, PredicateId, AssertionId>> current_index_entries() const;

  private:
    void remove_from_object_index(AssertionId id);

    std::unordered_map<EntityId, std::vector<AssertionId>> subject_index_;
    std::unordered_map<SubjectPredicateKey, std::vector<AssertionId>> current_index_;
    std::unordered_map<AssertionId, SubjectPredicateKey> assertion_keys_;
    std::unordered_map<EntityId, std::unordered_set<PredicateId>> predicate_index_;
    std::unordered_map<PredicateId, std::vector<AssertionId>> predicate_current_index_;
    std::unordered_map<EntityId, std::vector<std::pair<Timestamp, AssertionId>>> observed_time_index_;
    std::unordered_map<EntityId, std::vector<AssertionId>> object_index_;
    std::unordered_map<AssertionId, EntityId> assertion_object_;
};

} // namespace knk