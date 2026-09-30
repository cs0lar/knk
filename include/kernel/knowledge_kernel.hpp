#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "kernel/aggregate.hpp"
#include "kernel/assertion.hpp"
#include "kernel/catalog.hpp"
#include "kernel/ids.hpp"
#include "kernel/index_manager.hpp"
#include "kernel/kernel_command.hpp"
#include "kernel/kernel_result.hpp"
#include "kernel/provenance_log.hpp"
#include "kernel/query.hpp"
#include "kernel/query_engine.hpp"
#include "kernel/query_plan.hpp"
#include "kernel/schema.hpp"
#include "kernel/spill.hpp"
#include "kernel/status.hpp"
#include "kernel/storage_engine.hpp"
#include "kernel/time.hpp"
#include "kernel/value.hpp"

namespace knk {

class KnowledgeKernel {
  public:
    // ReadOnly (Phase 13) opens the root for querying alongside a live writer: it takes no lock, creates
    // nothing, and every mutating method below throws instead of writing. Two consequences worth knowing
    // before using it:
    //
    //  * A read-only kernel is a **snapshot as of its own construction**, not a live view. It replays the
    //    log once, in the constructor, exactly as a read-write kernel does; commits a writer makes
    //    afterwards are invisible to it until it is reopened.
    //  * It cannot repair derived state. If the persisted indexes are stale or corrupt it rebuilds them
    //    *in memory* and answers correctly, but leaves the files untouched -- so every read-only open of
    //    such a root pays a full replay until a writer opens it once and heals them.
    explicit KnowledgeKernel(StorageConfig config, OpenMode mode = OpenMode::ReadWrite);

    OpenMode mode() const;

    AssertionId commit(EntityId subject, PredicateId predicate, EntityId object, Timestamp valid_from,
                       Timestamp valid_to, Timestamp observed_at, double confidence);

    // Convenience wrapper over commit() for a caller that only has names/literals, not ids: interns
    // subject_name and predicate_name (idempotent, same as calling intern_entity/intern_predicate
    // directly), interns object via intern_value (a text Value is exactly what intern_entity would
    // produce for an object that's itself a named entity, so this one parameter covers both "object
    // is another named entity" and "object is a literal"), then commits with the resulting ids. Pure
    // composition of existing idempotent primitives -- no new storage, no new invariants. Closes the
    // gap flagged as out of scope back in Phase 5 ("name-based overloads of commit... may become
    // their own follow-up once the base catalog and payload store are in place").
    AssertionId commit_by_name(std::string_view subject_name, std::string_view predicate_name, const Value &object,
                               Timestamp valid_from, Timestamp valid_to, Timestamp observed_at, double confidence);

    // The largest batch commit_batch accepts. A bound, not a tuning knob: it caps how much a single
    // caller can buffer in memory (and how much a torn batch can leave half-applied) before the
    // kernel refuses, so an unbounded caller-supplied list can never become an unbounded write. A
    // caller with more than this splits into several batches, each its own durability boundary.
    static constexpr size_t MAX_BATCH_SIZE = 10'000;

    // Commits many new Active assertions under a single durability boundary: one fsync per underlying
    // log for the whole batch instead of one per assertion, which is what makes restating a field
    // across a whole population affordable. Returns the new ids in input order (entries take
    // consecutive ids), so a caller can record per-assertion provenance afterwards without a lookup
    // per assertion. Throws std::runtime_error if entries.size() exceeds MAX_BATCH_SIZE -- before
    // appending anything, so an over-sized batch burns no AssertionId and writes no record. An empty
    // batch is a no-op returning an empty vector, touching no log.
    //
    // Durability is prefix-shaped, not all-or-none, and the distinction is deliberate. The log's
    // append-only format has no multi-record commit marker, and adding one would mean a new record
    // type and a format version bump for every reader -- so rather than claim atomicity it cannot
    // deliver, this guarantees the strongest thing the format actually supports: a crash mid-batch
    // leaves the first k entries durable for some 0 <= k <= entries.size(), never a gap and never a
    // reordering. Because ids are consecutive and assigned in input order, k is exactly recoverable
    // afterwards -- the surviving prefix is a contiguous id range, so a caller resumes at input index
    // k rather than guessing. The three index logs and the checkpoint are written after the assertion
    // log, so a crash between them leaves the checkpoint behind the log and startup rebuilds every
    // index from it, exactly as it already does for a torn single commit.
    //
    // Deliberately only plain appends: no supersession, retraction, or hypothesis entries, and no
    // per-entry provenance. Each of those has target validation or a second durable write that would
    // have to interleave with the batch's single boundary, which is a different (and much less
    // obviously correct) feature than the one this solves.
    std::vector<AssertionId> commit_batch(const std::vector<PendingAssertion> &entries);

    // commit_batch's name-based overload, standing to it exactly as commit_by_name stands to commit:
    // interns each entry's subject name, predicate name, and object Value (all idempotent, so names
    // already in the catalog cost nothing), then commits the resolved entries through commit_batch
    // itself. Every guarantee above carries over unchanged -- ids in input order, per-entry valid
    // time, MAX_BATCH_SIZE, prefix-shaped durability.
    //
    // One thing does not carry over: interning happens *before* the batch's durability boundary, and
    // each genuinely new name is its own catalog append and fsync. So a batch of all-new names costs
    // one fsync per new name plus the batch's own, while the case this exists for -- restating a
    // field for subjects the kernel already knows, under one predicate -- interns nothing new for the
    // subjects and at most one new predicate. A crash partway through interning is harmless rather
    // than partial: catalog entries are id/name mappings with no assertion attached yet, interning is
    // idempotent, so a retry reuses the same ids and simply commits again. The size check runs before
    // any interning, so a rejected batch interns nothing at all.
    std::vector<AssertionId> commit_batch_by_name(const std::vector<PendingNamedAssertion> &entries);

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

    // limit caps the number of returned assertions to the given count; 0 (the default) means no
    // cap, matching prior behavior. Order is otherwise unchanged (whatever assertions_for_subject
    // already returns), so limit == N means "the first N of that order," not "the N most recent" --
    // a caller wanting the tail should look at changes_since's newest_first instead.
    std::vector<Assertion> assertions_for_subject(EntityId subject, size_t limit = 0) const;

    std::vector<Assertion> current(EntityId subject) const;

    // Read-side mirror of commit_by_name: resolves subject_name via find_entity and delegates to
    // current(). Deliberately looks up rather than interns -- unlike commit_by_name, a read-only query
    // must not spuriously mint a new entity/id for a name that was never committed (a typo or an
    // as-yet-unknown subject). Returns an empty vector for a name that was never interned, matching
    // current()'s existing behavior for a subject id with no current facts.
    std::vector<Assertion> current_by_name(std::string_view subject_name) const;

    // Reverse-direction counterpart to current(subject): every currently active, open-ended
    // assertion where the given entity is the object rather than the subject -- e.g. "who currently
    // works at Acme." object is resolved through Catalog first, exactly like current(subject)
    // resolves subject, so a caller holding an id later merged away still gets the surviving id's
    // results.
    std::vector<Assertion> current_by_object(EntityId object) const;

    // Kernel-wide counterpart to current(subject): every currently active, open-ended assertion for
    // the given predicate, any subject -- e.g. "every WORKS_AT relationship." No entity resolution
    // needed -- merge_entities only ever redirects EntityIds, and predicates are not subject to it.
    std::vector<Assertion> current_by_predicate(PredicateId predicate) const;

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

    // limit caps the returned count, same convention as assertions_for_subject's limit: 0 means no
    // cap, and the cap applies after the existing id-ascending (commit-order) sort, so it's "the
    // first N," not "the N most recent."
    std::vector<Assertion> commit_history(EntityId subject, PredicateId predicate, size_t limit = 0) const;

    // Kernel-wide answer to "what changed since t": every assertion (any subject, any predicate)
    // with observed_at >= observed_since, sorted by observed_at then id for a stable order among
    // ties (or the reverse of that order when newest_first is set -- see below). Status-agnostic
    // like commit_history -- new commits, supersessions, retractions, and hypotheses all count as a
    // "change" -- but unlike commit_history/observed_time_timeline this does not take a subject or
    // predicate, since the whole point is discovering what changed without already knowing where to
    // look. A straight scan over assertions_, not an index lookup: there is no persisted global
    // observed-time ordering (IndexManager's observed-time index is per-subject), and Phase 9's
    // Performance Rules require a demonstrated bottleneck plus a benchmark before adding one.
    //
    // limit caps the returned count (0, the default, means no cap -- unchanged prior behavior).
    // newest_first, when true, reverses the sort so the most-recently-observed change (and, among
    // ties, the highest id) comes first, then limit is applied to that order -- e.g.
    // changes_since(0, /*limit=*/1, /*newest_first=*/true) answers "what's the single latest change"
    // without the caller reading and discarding the rest of the log to find it. limit is still
    // applied after the same full scan+sort as always: this fixes what a caller has to read, not
    // the kernel's internal work, which stays a Phase 9 concern gated behind a benchmark.
    std::vector<Assertion> changes_since(Timestamp observed_since, size_t limit = 0, bool newest_first = false) const;

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

    // record_provenance's batch counterpart, and the other half of commit_batch: one fsync on the
    // provenance log for the whole list rather than one per record. This is what makes attaching
    // provenance to a batch affordable -- commit_batch returns its ids in input order precisely so a
    // caller can zip them with sources here, and without this the per-record fsyncs cost several
    // times the batch they describe.
    //
    // Deliberately a separate call rather than per-entry provenance inside commit_batch. Provenance
    // written inside the batch could, after a crash, reference assertion ids the batch's durable
    // prefix never committed -- and startup replay of provenance.log fills its map without validating
    // targets, so those would linger as provenance for assertions get() does not know. Recording
    // afterwards means every target is already committed, so the validation below checks against real
    // state.
    //
    // Every target is validated before anything is appended, so one bad id rejects the whole call
    // without writing a record or mutating the in-memory map -- the same all-or-nothing property
    // record_provenance has, extended to the list. Beyond that check, durability is prefix-shaped
    // exactly like commit_batch's: a crash can leave the first k records durable. That is benign
    // here in a way it is not for assertions -- provenance.log is append-only and last-writer-wins per
    // assertion, so re-running the same call after a crash simply re-records the missing tail.
    // Bounded by MAX_BATCH_SIZE, checked before any validation or append. An empty list is a no-op.
    void record_provenance_batch(const std::vector<ProvenanceRecord> &records);

    std::optional<ProvenanceRecord> provenance_for(AssertionId assertion_id) const;

    // Batch reads (#55): the read-side counterparts of the write batches, one per single-id resolver.
    // Reads like current_by_predicate hand back records whose subject/object/predicate are ids, so a
    // caller rendering them resolves ids one at a time -- one boundary round trip per id when calling
    // through the MCP server. These turn that into one call.
    //
    // Answers come back in input order, one slot per id, and each slot is exactly what the single call
    // answers for that id -- each batch is literally the single resolver called once per id, so the two
    // cannot drift. That includes nullopt wherever the single call answers nullopt: an id that was never
    // interned (or never had provenance recorded), and entity_name for an entity whose Value is not
    // Text. Duplicate ids are answered once per occurrence, and merge redirects are not followed, both
    // exactly as the single calls behave.
    //
    // Deliberately nullopt per slot rather than rejecting the call on an unknown id, which is what
    // record_provenance_batch does: that write validates its targets because an unknown one would
    // persist a dangling record, but a read has nothing to protect, and the single resolvers already
    // answer an unknown id with nullopt rather than throwing. Rejecting here would make a batch mean
    // something different from N singles.
    //
    // Bounded by MAX_BATCH_SIZE, checked before anything is read; an empty list answers an empty list.
    std::vector<std::optional<std::string>> entity_name_batch(const std::vector<EntityId> &ids) const;

    std::vector<std::optional<Value>> entity_value_batch(const std::vector<EntityId> &ids) const;

    std::vector<std::optional<std::string>> predicate_name_batch(const std::vector<PredicateId> &ids) const;

    std::vector<std::optional<ProvenanceRecord>> provenance_for_batch(const std::vector<AssertionId> &ids) const;

    // Executes a reified Query (Phase 10; see include/kernel/query.hpp and AGENTS.md's "Query Engine"
    // section). Every query method above is expressible as a Query returning identical rows -- asserted
    // by the parity tests in tests/query_engine_tests.cpp -- so this adds reach, not a second
    // interpretation of the bitemporal rules. The methods above remain the documented way to ask the
    // simple questions; this is for the shaped ones they cannot express (several filters at once,
    // explicit status sets, deterministic ordering, paging).
    //
    // Read-only: no durable state is written, and nothing here participates in a commit path. Throws
    // std::runtime_error for an unknown Query::ir_version; every other malformed-looking query is just a
    // filter that matches nothing.
    QueryResult query(const Query &query) const;

    // The plan query() would follow, without running it (Phase 16). Produced by the same call the
    // executor makes, so an explanation cannot describe something other than what will happen. An
    // analytics caller needs this to predict what a query costs, and to notice when a change in the
    // corpus has silently changed the plan underneath them.
    QueryPlan explain_query(const Query &query) const;

    // Writes a query's rows to `directory / token` as columns and returns the descriptor (Phase 17): the
    // way out of the JSON row ceiling for a result an analytics consumer wants whole. `token` empty means
    // one is generated.
    //
    // Deliberately **not** guarded by require_writable: a read-only kernel is exactly the analytics case,
    // and the spill directory is the caller's, never under the storage root -- so a reader can produce
    // results while still writing nothing to the root it opened.
    //
    // limit == 0 means MAX_SPILL_ROWS rather than the JSON ceiling; an explicit limit is honoured and
    // capped to it. offset applies as it does for a page.
    SpillDescriptor spill_query(const Query &query, const std::filesystem::path &directory,
                                const std::string &token = {}) const;

    // Aggregates rows in a single streaming pass (Phase 12): counts, sums and extrema over groups, with
    // the same selection surface a row query uses, so "how many" and "which" can never disagree about
    // what is current. Memory is bounded by the number of groups, not the number of matching rows.
    //
    // Throws std::runtime_error for the caller mistakes described on QueryEngine::aggregate, including
    // an aggregate that would produce more groups than its cap allows -- truncating an aggregate would
    // hand back a wrong answer that looks like a right one.
    AggregateResult aggregate(const AggregateQuery &query) const;

    // Schema discovery (Phase 18): every interned predicate with the rows it currently has. The one thing
    // an MCP client cannot learn from the query tool's JSON Schema is which predicate *names* this store
    // actually uses, and guessing produces empty results that look like absent facts.
    std::vector<PredicateSummary> describe_predicates() const;

    // Corpus shape: row and entity counts, per-status counts, and the observed/valid-time span. Counts
    // come from the indexes where they are exact and free; the status counts and time span are one linear
    // pass over assertions_, which is why this is a discovery call and not something to put in a loop.
    CorpusSummary describe_corpus() const;

    // Merges absorb into keep: a one-way, append-only redirect durably recorded in EntityMergeLog,
    // then applied to the in-memory Catalog. Assertions are never rewritten by a merge -- assertions_
    // keeps whatever EntityId was originally committed, and resolve_entity (plus every query-path
    // method below that takes a caller-supplied EntityId) follows the redirect transitively at the
    // query boundary instead. merged_at is caller-supplied, matching how observed_at/recorded_at are
    // supplied elsewhere -- the kernel never reads a wall clock, keeping replay deterministic. A merge
    // that turns out wrong is corrected by a later merge_entities call, never by mutating this one.
    void merge_entities(EntityId keep, EntityId absorb, Timestamp merged_at);

    // Resolves id through any recorded entity-merge redirects, transitively, to its canonical
    // surviving id. Identity for an id that was never absorbed into another.
    EntityId resolve_entity(EntityId id) const;

    // Compaction, not deletion: moves every already-rolled-from assertion-log segment entirely before
    // assertion_id out of the hot working set and into segments/archive/. This never shrinks queryable
    // history -- read_all/read_after (and therefore every audit/timeline query) still see archived
    // segments exactly as before. True, irreversible erasure is an explicit non-goal, not deferred work.
    void archive_segments_before(AssertionId assertion_id);

    // Executes a reified command by dispatching 1:1 to the mirrored public method above and wrapping
    // its return value in a KernelResult. This is a thin, closed dispatch boundary, not new business
    // logic -- see include/kernel/kernel_command.hpp. Non-const because the command set includes
    // mutating operations (commit, intern_*, record_provenance, ...).
    KernelResult execute(const KernelCommand &command);

  private:
    // Throws when this kernel was opened ReadOnly, naming the operation. Called at the top of every
    // mutating method, before any in-memory state is touched: intern_document, for instance, allocates
    // an id before it reaches storage, so relying on the storage layer's own guard would leak one.
    void require_writable(const char *operation) const;

    // Bundles the state a query runs against, including the columnar projection when it is usable.
    QuerySource query_source() const;

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

    // Replayed, effective status per row, parallel to assertions_ and to the columnar store's rows. It
    // exists because the columns are a verbatim projection of the log: their status byte is the one the
    // record was appended with, so a superseded row still reads Active there (see column_store.hpp). One
    // byte per row, rebuilt at the end of construction and maintained by apply()/mark_*.
    std::vector<uint8_t> effective_status_;

    // Names generated spills. A counter rather than a clock, because the kernel never reads one; the
    // generator also skips tokens already on disk, so a fresh process cannot collide with an old result.
    mutable size_t next_spill_ = 1;

    // Stateless: the assertions, indexes and catalog a query runs against are passed to execute()
    // rather than held, which is what keeps KnowledgeKernel safe to move (see query_engine.hpp).
    QueryEngine query_engine_;
};
} // namespace knk
