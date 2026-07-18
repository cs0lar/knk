#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "kernel/assertion.hpp"
#include "kernel/catalog.hpp"
#include "kernel/ids.hpp"
#include "kernel/index_manager.hpp"
#include "kernel/kernel_command.hpp"
#include "kernel/kernel_result.hpp"
#include "kernel/provenance_log.hpp"
#include "kernel/status.hpp"
#include "kernel/storage_engine.hpp"
#include "kernel/time.hpp"
#include "kernel/value.hpp"

namespace knk {

class KnowledgeKernel {
  public:
    explicit KnowledgeKernel(StorageConfig config);

    AssertionId commit(EntityId subject, PredicateId predicate, EntityId object, Timestamp valid_from,
                       Timestamp valid_to, Timestamp observed_at, double confidence);

    AssertionId commit_retraction(EntityId subject, PredicateId predicate, EntityId object, Timestamp valid_from,
                                  Timestamp valid_to, Timestamp observed_at, double confidence,
                                  AssertionId retracts_id);

    AssertionId commit_superseding(EntityId subject, PredicateId predicate, EntityId object, Timestamp valid_from,
                                   Timestamp valid_to, Timestamp observed_at, double confidence,
                                   AssertionId supersedes_id);

    // Commits a labeled, machine-suggested fact: mirrors commit()'s signature exactly, tags the
    // record AssertionStatus::Hypothesis instead of Active, and always records provenance for it (an
    // unsourced hypothesis is a contradiction in terms for this design, so source/recorded_at/method
    // are required, not optional). Internally this is commit() followed by record_provenance() -- no
    // new storage or durability mechanism. Hypothesis-status records are excluded from current/
    // valid_at/known_at/valid_at_known_at by the same status check those methods already use; they
    // remain visible via hypotheses_for, commit_history, and explain. Promotion is not a separate
    // primitive -- promote a hypothesis via ordinary commit_superseding, same as any other assertion.
    AssertionId commit_hypothesis(EntityId subject, PredicateId predicate, EntityId object, Timestamp valid_from,
                                  Timestamp valid_to, Timestamp observed_at, double confidence, EntityId source,
                                  Timestamp recorded_at, std::string method);

    void apply(const Assertion &assertion);

    // Persists a full snapshot of the current in-memory assertions_ so a future startup can skip
    // re-parsing the portion of assertions.log it covers. Explicit/caller-triggered only -- there
    // is no automatic cadence, so commit-path latency is unaffected.
    void write_snapshot();

    void mark_superseded(AssertionId superseded_id);

    void mark_retracted(AssertionId retracted_id);

    std::optional<Assertion> get(AssertionId id) const;

    std::vector<Assertion> assertions_for_subject(EntityId subject) const;

    std::vector<Assertion> current(EntityId subject) const;

    // Mirrors current(subject) but selects Hypothesis-status records instead of Active ones. Reads
    // off the subject index (like commit_history/valid_at), not the current-state index -- hypotheses
    // are never added to that index, since is_current_assertion requires Active status.
    std::vector<Assertion> hypotheses_for(EntityId subject) const;

    // Bounded local graph traversal: breadth-first from subject, following current (Active,
    // open-ended) assertion edges in both directions -- subject's own assertions (outgoing) and
    // assertions where subject is the object (incoming, via the in-memory reverse index). Returns a
    // flat, deduplicated set of reachable EntityIds, not paths, capped at max_hops hops. Feature
    // extraction for an external prediction/causal-inference tool, not a general graph query
    // language -- see AGENTS.md's "Do Not Do Yet" narrowing for Phase 7.
    std::vector<EntityId> neighbors(EntityId subject, size_t max_hops = 1) const;

    // The PredicateIds currently active for a single subject -- which relationship types co-occur on
    // the same entity right now. Deliberately per-subject, not a cross-subject association join (the
    // "no joins" restriction on this phase's graph-traversal exception): an external tool aggregating
    // co-occurrence across many subjects calls this once per subject and does that aggregation itself.
    std::vector<PredicateId> co_occurring_predicates(EntityId subject) const;

    std::vector<Assertion> valid_at(EntityId subject, Timestamp valid_time) const;

    std::vector<Assertion> known_at(EntityId subject, Timestamp observed_time) const;

    std::vector<Assertion> valid_at_known_at(EntityId subject, Timestamp valid_time, Timestamp observed_time) const;

    std::vector<Assertion> valid_time_timeline(EntityId subject, PredicateId predicate) const;

    std::vector<Assertion> observed_time_timeline(EntityId subject, PredicateId predicate) const;

    std::vector<Assertion> commit_history(EntityId subject, PredicateId predicate) const;

    // Walks the supersession/retraction chain from the given assertion back to its root, following
    // supersedes_id/retracts_id one hop at a time. Returns the chain newest-first (the given
    // assertion, then the one it superseded/retracted, ... , down to the original that links no
    // further). An unknown or zero id returns an empty vector. This is the concrete answer to "why
    // does the kernel believe this" -- the caller can resolve each hop's provenance via
    // provenance_for once that lands.
    std::vector<Assertion> explain(AssertionId id) const;

    // Detects overlapping active assertions for the same subject/predicate: every unordered pair of
    // Active assertions with a different object whose valid-time intervals overlap (half-open
    // [valid_from, valid_to), with OPEN_ENDED meaning unbounded). Pure read-side over the existing
    // subject index -- no new storage. Superseded/retracted assertions are already resolved and are
    // never reported as conflicts.
    std::vector<std::pair<Assertion, Assertion>> find_conflicts(EntityId subject, PredicateId predicate) const;

    EntityId intern_entity(std::string_view name);

    EntityId intern_value(const Value &value);

    PredicateId intern_predicate(std::string_view name);

    std::optional<EntityId> find_entity(std::string_view name) const;

    std::optional<EntityId> find_value(const Value &value) const;

    std::optional<PredicateId> find_predicate(std::string_view name) const;

    std::optional<std::string> entity_name(EntityId id) const;

    std::optional<Value> entity_value(EntityId id) const;

    std::optional<std::string> predicate_name(PredicateId id) const;

    EntityId intern_document(std::span<const std::byte> content);

    std::optional<std::vector<std::byte>> document_content(EntityId id) const;

    // Records which source (an EntityId, interned in Catalog like any other entity) produced a given
    // assertion, and by what method. recorded_at is caller-supplied, matching how observed_at is
    // supplied to commit -- the kernel deliberately never reads a wall clock, so provenance replay
    // stays deterministic. Validates that assertion_id refers to an existing assertion before the
    // durable append, so a failed call never persists a dangling provenance record.
    void record_provenance(AssertionId assertion_id, EntityId source, Timestamp recorded_at, std::string method);

    std::optional<ProvenanceRecord> provenance_for(AssertionId assertion_id) const;

    // Executes a reified command by dispatching 1:1 to the mirrored public method above and wrapping
    // its return value in a KernelResult. This is a thin, closed dispatch boundary, not new business
    // logic -- see include/kernel/kernel_command.hpp. Non-const because the command set includes
    // mutating operations (commit, intern_*, record_provenance, ...).
    KernelResult execute(const KernelCommand &command);

  private:
    void restore_assertion(const Assertion &assertion);

    AssertionId next_id_ = 1;

    StorageEngine storage_;

    IndexManager index_manager_;

    Catalog catalog_;

    // Provenance keyed by AssertionId. Authoritative like Catalog (nothing in assertions.log encodes
    // it), replayed from provenance.log at startup. The log is append-only, so a later record for the
    // same assertion supersedes an earlier one; the map keeps the last one replayed.
    std::unordered_map<AssertionId, ProvenanceRecord> provenance_;

    std::vector<Assertion> assertions_;
};
} // namespace knk
