# AGENTS.md

## Project: Knowledge Kernel

This repository implements a C++ Knowledge Kernel: a bitemporal assertion store with graph - shaped indexes, designed as the persistent core for conversational, temporal, and provenance - aware knowledge systems.

The primitive is not a row, document, or graph edge. The primitive is an **Assertion** : a versioned temporal claim about the world.

The long - term goal is to build a low - latency embedded knowledge engine that can store, retrieve, replay, index, compact, replicate, and reason over assertions.

-- -

## Core Concept

An assertion represents a temporally scoped claim :

```cpp
subject --predicate-- > object
```

with :

```cpp
valid_from
valid_to
observed_at
confidence
status
supersedes_id
retracts_id
```

The system distinguishes :

* **Valid time** : when the assertion is true in the world.
* **Observed time** : when the system learned or recorded the assertion.
* **Commit order** : the order in which knowledge changes entered the kernel.

The assertion log is append - only. Assertions are not overwritten in place. Updates are represented by new committed records that supersede, retract, or refine previous assertions.

-- -

## Current Roadmap

Agents must follow this roadmap and should not jump ahead unless explicitly instructed.

### Phase 1 — In-memory temporal database ✅

Implemented or in progress :

* Assertions
* Bitemporal semantics
* Tests

Core responsibilities :

* Define assertion model.
* Support in - memory commit.
* Query by current state.
* Query by valid time.
* Query by observed time.
* Support superseded and retracted assertions.
* Maintain clear tests for temporal semantics.

### Phase 2 — Storage engine ✅

Current focus :

* ✅ AssertionLog
* ✅ StorageEngine
* ✅ Recovery from assertion log on `KnowledgeKernel` startup
* ✅ Durable-before-visible commit ordering
* ✅ IndexManager scaffold compiled and directly tested
* ✅ Initial IndexManager integration into `KnowledgeKernel`
* ✅ Public supersession and retraction commit APIs

The current source of truth is the append - only assertion log.

Startup should :

```text
read assertion log
→ replay records in commit order
→ reconstruct in - memory assertions
→ rebuild indexes
```

Runtime commit should :

```text
construct assertion
→ append to durable log
→ apply to in - memory state
→ update indexes
```

Never update memory before the log write succeeds.

Current implementation status :

* Phase 1 in-memory temporal queries are implemented in `KnowledgeKernel`.
* `commit`, `commit_superseding`, `commit_retraction`, failed durable append behavior, `get`, subject lookup, current open-ended lookup, valid-time lookup, observed-time lookup, valid-at-known-at lookup, timeline/history lookups, and recovery across kernel instances are covered by standard-library tests.
* Assertions are appended to `assertions.log` through `StorageEngine` and `AssertionLog` before being applied to memory.
* Startup recovery reads the assertion log and replays records in commit order.
* Replay restores `next_id_` by advancing past the largest replayed assertion ID.
* Recovery tests currently live in `tests/knowledge_kernel_tests.cpp`, not a separate `tests/recovery_tests.cpp`.
* `AssertionLog` writes a simple size-prefixed binary record using raw `Assertion` bytes. It validates record size (throwing `std::runtime_error` on a mismatch) and ignores an incomplete trailing assertion payload; both behaviors are covered by `tests/assertion_log_tests.cpp` (`assertion_log_rejects_invalid_record_size`, `assertion_log_ignores_incomplete_trailing_record`).
* `IndexManager` is scaffolded in `include/kernel/index_manager.hpp` and `src/index_manager.cpp`, compiled into the `kernel` target, and covered by `tests/index_manager_tests.cpp`.
* `IndexManager` currently maintains subject, predicate, current assertion ID, and observed-time indexes. Current assertion indexing includes only active open-ended assertions. Retraction records are kept in the subject/audit index but are not indexed as current facts. Predicate indexing tracks predicates that still have at least one current assertion for a subject.
* `IndexManager::observed_before(subject, t)` (first Phase 3 index, added ahead of the rest of that phase) returns assertion IDs for a subject with `observed_at <= t`, kept sorted by `observed_at` via sorted insertion in `add`. Like the subject index, it retains superseded/retracted/retraction records; status filtering stays the caller's responsibility. `KnowledgeKernel::known_at` and `KnowledgeKernel::valid_at_known_at` now query this index instead of scanning all of a subject's assertions.
* `IndexManager::mark_superseded` and `IndexManager::mark_retracted` remove assertion IDs from the current index without removing them from the subject/audit index. They remove a subject/predicate entry from the predicate index only when no current assertions remain for that key.
* `KnowledgeKernel` now owns an `IndexManager` and routes subject lookup, current lookup, valid-time lookup, observed-time lookup, and valid-at-known-at lookup through assertion IDs returned by `IndexManager`.
* `KnowledgeKernel` no longer owns separate `subject_index_` and `current_index_` maps.
* The public current lookup is `current(subject)` and returns active open-ended assertions for all current predicates of that subject by using `IndexManager::predicates_for_subject` and `IndexManager::current_assertions`.
* `KnowledgeKernel` constructs a fresh internal `IndexManager` during startup and rebuilds it from the assertion log; externally supplied index state is not accepted as a source of truth.
* `KnowledgeKernel::commit_superseding` commits an active replacement assertion with `supersedes_id` and marks the target assertion as `Superseded`.
* `KnowledgeKernel::commit_retraction` commits an assertion-like audit record with status `Retraction` and `retracts_id`, and marks the target assertion as `Retracted`.
* Retraction targets are validated before append so failed retractions do not persist invalid log records or burn assertion IDs.
* Timeline/history query support currently exists as three public methods: `valid_time_timeline(subject, predicate)` returns active assertions sorted by valid time, `observed_time_timeline(subject, predicate)` returns active assertions sorted by observed time, and `commit_history(subject, predicate)` returns all recorded assertions for the subject/predicate sorted by assertion ID.
* Core types, `AssertionStatus`, storage classes, and `KnowledgeKernel` now live in the `knk` namespace.
* Verified on 2026-07-09: `cmake --build build && ctest --test-dir build --output-on-failure` passes with the current CMake targets after replay and conflicting-active-assertion regression tests.

### Phase 3 — Persistent indexes/current phase

Future work :

* ✅ Predicate index (derived from the persisted current-state index; see implementation status below)
* ✅ Current - state index (persisted to disk; see implementation status below)
* ✅ Observed - time index (persisted to disk; see implementation status below)

Indexes are derived acceleration structures. They must be rebuildable from the assertion log.

If persistent indexes are missing or corrupted, the kernel should still recover from the log.

All four Phase 3 indexes are now durable. This closes out the "remaining Phase 3 work" note that used to be
here.

Current implementation status :

* Three index logs are persisted alongside `assertions.log`, each using the same size-prefixed binary record
 format (missing file -> empty; mismatched size prefix -> throws `std::runtime_error`; incomplete trailing
 record -> ignored) and each exposing `overwrite_all` for self-heal only — everything else is append-only:
 * `ObservedTimeIndexLog` (`include/kernel/observed_time_index_log.hpp`, `src/observed_time_index_log.cpp`)
   persists `{subject, observed_at, assertion_id}` to `indexes/observed_time.idx`.
 * `SubjectIndexLog` (`include/kernel/subject_index_log.hpp`, `src/subject_index_log.cpp`) persists
   `{subject, assertion_id}` to `indexes/subject.idx`.
 * `CurrentIndexLog` (`include/kernel/current_index_log.hpp`, `src/current_index_log.cpp`) persists
   `{subject, predicate, assertion_id, active}` to `indexes/current.idx`. Unlike the other two, entries here
   can later become false (the current-state index removes an entry when an assertion is superseded or
   retracted), so this log is append-only in the tombstone sense: becoming current appends an `active = true`
   record, ceasing to be current appends a second `active = false` record for the same `(subject, predicate,
   assertion_id)` rather than mutating the first record in place.
 * There is deliberately no separate persisted predicate-index file. `predicate_index_` is only ever a
   projection of `current_index_`'s keys (which subjects have at least one current predicate), so replaying
   `current.idx` alone is sufficient to rebuild both `current_index_` and `predicate_index_` together — see
   `IndexManager::restore_current_index_entry`.
* `StorageEngine` owns all three index logs alongside `AssertionLog` and exposes matching
 `append_*`/`load_*`/`rewrite_*` methods for each (`append_observed_time_entry`/`load_observed_time_index`/
 `rewrite_observed_time_index`, and the `subject`/`current` equivalents).
* `IndexManager` has exactly one assertion-indexing entry point, `add(const Assertion&)`, used for every commit
 and every full-replay record — there is no `add_without_*` variant. It is a three-line composition of three
 narrower `restore_*` primitives (`restore_current_index_entry`, `restore_subject_entry`,
 `restore_observed_time_entry`), each of which operates on raw persisted-record fields rather than a full
 `Assertion` and is also used standalone to seed an index directly from its own file at startup. A free
 function `is_current_assertion(const Assertion&)` (an assertion is current iff `status == Active` and
 `valid_to == OPEN_ENDED`) is shared between `IndexManager::add` and `KnowledgeKernel`'s commit paths so the
 eligibility rule lives in one place. `IndexManager::observed_time_entries()`, `subject_index_entries()`, and
 `current_index_entries()` each return a flat snapshot of their index (the last one only ever contains
 currently-active entries, since removed ones are erased from `current_index_` in memory) for self-heal
 rewrites.
* `KnowledgeKernel` likewise has exactly one replay-application method, `apply(const Assertion&)`, used for both
 the commit path and full-log replay; it always calls `index_manager_.add`. A separate private
 `restore_assertion(const Assertion&)` rebuilds only `assertions_`/`next_id_`/superseded-or-retracted status —
 it never touches `IndexManager` — and is used solely on the fast startup path described below.
* `KnowledgeKernel`'s constructor tries to load all three persisted index files independently (each in its own
 try/catch, so a diagnostic could in principle identify which one is corrupt), but treats them as all-or-
 nothing: if any one is missing or corrupt, the whole partially-restored `IndexManager` is discarded and
 `assertions.log` is replayed in full through `apply` (rebuilding every index uniformly, even ones that loaded
 fine), after which fresh snapshots of all three are written via `rewrite_observed_time_index`/
 `rewrite_subject_index`/`rewrite_current_index` so the next startup can take the fast path. If all three
 loaded successfully, `assertions.log` is still walked once (to rebuild `assertions_`, `next_id_`, and
 superseded/retracted status, none of which live in the index files), but via `restore_assertion` — the
 already-restored indexes are never re-populated. This all-or-nothing choice trades a bit of redundant rebuild
 work on partial corruption for avoiding a combinatorial `apply`/`add` variant per subset of indexes; it is a
 deliberate Phase 3 simplification, not a performance claim (see "Performance Rules").
* `commit`, `commit_superseding`, and `commit_retraction` each append to the observed-time and subject index
 logs (as before), and now also append to the current index log, before calling `apply` — durable-before-
 visible covers all three persisted indexes. `commit` appends one current-index record for the new assertion
 (`active = is_current_assertion(assertion)`). `commit_superseding` appends one such record for the new
 assertion plus an `active = false` tombstone for the superseded target. `commit_retraction` appends only the
 `active = false` tombstone for the retracted target, since a `Retraction`-status record can never itself be
 current.
* Covered by `tests/observed_time_index_log_tests.cpp`, `tests/subject_index_log_tests.cpp`, and
 `tests/current_index_log_tests.cpp` (each: append/read round trip, missing file, invalid record size,
 incomplete trailing record, `overwrite_all`); additions to `tests/index_manager_tests.cpp`
 (`is_current_assertion_requires_active_status_and_open_ended_valid_to`,
 `restore_current_index_entry_reproduces_current_index_out_of_band`,
 `restore_current_index_entry_removal_of_unknown_assertion_is_a_noop`, `restore_subject_entry_...`,
 `restore_observed_time_entry_...`, and the three `*_entries_returns_a_flat_snapshot_of_the_index` tests); and
 additions to `tests/knowledge_kernel_tests.cpp` covering cross-kernel restore, corruption fallback plus
 self-heal for each of the three index files individually
 (`corrupt_observed_time_index_falls_back_to_replay_and_self_heals`,
 `corrupt_subject_index_falls_back_to_replay_and_self_heals`,
 `corrupt_current_index_falls_back_to_replay_and_self_heals`) and in combination
 (`corrupt_all_persisted_indexes_falls_back_to_full_replay_and_self_heals`), and
 `superseded_assertion_remains_excluded_from_current_after_restart` (the two-tombstone supersede path survives
 a kernel restart).

### Phase 4 — Storage engine internals

Future work :

* ✅ Segment files (`AssertionLog` only; see implementation status below)
* WAL (no separate task — `AssertionLog`'s append-only + fsync-before-return + durable-before-visible
 ordering already is a WAL in every functional sense; nothing left to add)
* ✅ Checksums (per-record CRC32; see implementation status below)
* ✅ Crash recovery (fsync durability, tail-tolerant corruption policy, index checkpoint; see implementation
 status below)
* ✅ Snapshots (explicit full-replace snapshot of `assertions_`; see implementation status below)

At this phase, the log should evolve from a simple file into a segmented storage subsystem with integrity checks and controlled recovery.

Current implementation status :

* All four logs (`AssertionLog`, `SubjectIndexLog`, `CurrentIndexLog`, `ObservedTimeIndexLog`) share an identical
 framed format: an 8-byte file header (`"KNK1"` magic + `uint32_t` format version, written once per file)
 followed by repeated `[uint32_t record_size][raw struct bytes][uint32_t crc32]` frames. The shared CRC-32
 (IEEE 802.3 polynomial) implementation lives in `include/kernel/checksum.hpp`/`src/checksum.cpp`; each log
 otherwise keeps its own read/write loop rather than sharing a generic framer, matching the existing duplication
 style across the four log types.
* Corruption policy (revised by the Crash recovery work below from the original all-throw checksums design): a
 mismatched `record_size` always throws `std::runtime_error` (framing is unrecoverable once size is wrong). A
 torn header (fewer than 8 bytes, from a crash on the very first-ever append) is treated the same as an empty
 file, not thrown. A checksum mismatch on a fully-present frame is tail-tolerant: silently dropped if nothing
 follows it in the file (indistinguishable from a crash mid-append, same treatment as a short/truncated
 trailing frame), but still throws if valid-length data follows it (data can't validly follow a torn write, so
 that is unambiguous real corruption). A full 8-byte header with the wrong magic/version still always throws
 (a torn write cannot produce a full-length-but-wrong header).
* Durability: every `append()` closes its stream and then fsyncs the file (`knk::fsync_file`,
 `include/kernel/durability.hpp`) before returning, so a commit isn't durable until it reaches physical disk,
 not just the OS page cache. `overwrite_all()` (the three index logs' self-heal rewrite) writes via
 `knk::write_file_atomically` (temp file, fsync, atomic rename, fsync parent directory), so a crash mid-rewrite
 can never leave a half-written index file. Both are POSIX-only, an accepted limitation for early local
 development, same treatment as the raw-struct-serialization limitation already documented.
* Index checkpoint: `indexes/checkpoint` (`include/kernel/index_checkpoint.hpp`/`src/index_checkpoint.cpp`)
 closes a cross-log atomicity gap found while implementing this: `commit`/`commit_superseding`/
 `commit_retraction` append to `assertions.log` before the three index logs, so a crash in between leaves
 `assertions.log` with a record none of the index logs know about — and none of their `read_all()` calls throw
 in that case (the entry is simply missing, indistinguishable from never existing). The checkpoint persists the
 highest `AssertionId` whose index writes are confirmed complete, written as the last step of each commit's
 storage-append sequence; unlike the four data logs it never throws on read (missing/corrupt both degrade to
 `0`, since it's purely a startup-fast-path hint, not authoritative data). `KnowledgeKernel`'s constructor
 trusts the persisted indexes only if every index file loaded cleanly *and* the checkpoint matches the highest
 id in `assertions.log`; otherwise it takes the existing full-replay-and-self-heal path unchanged, additionally
 persisting the new checkpoint afterward.
* `AssertionLog` reads remain unwrapped in any recovery path — a corrupted assertion log is still a fatal,
 uncaught startup error, *except* when the corruption is tail-tolerant (a torn trailing write), which is now
 silently and safely dropped like the index logs. Non-tail corruption stays fatal deliberately: `assertions.log`
 is the one source of truth, so there is nothing to rebuild it from, and silently discarding real history would
 be worse than refusing to start.
* See `docs/storage_format.md` for the full format spec, durability model, and checkpoint format. Covered by
 `tests/checksum_tests.cpp`, `tests/durability_tests.cpp`, and `tests/index_checkpoint_tests.cpp`; each of the
 four log test files has `..._recovers_partial_header_as_empty_log`,
 `..._recovers_tail_checksum_mismatch_as_torn_write`, and
 `..._rejects_checksum_mismatch_when_followed_by_more_data` tests; and
 `crash_between_assertion_append_and_index_append_recovers_via_checkpoint` in
 `tests/knowledge_kernel_tests.cpp` exercises the cross-log atomicity gap directly (bypassing `commit()` via a
 raw `StorageEngine::append_assertion` call, then confirming a reopened kernel still surfaces the assertion via
 `assertions_for_subject`/`current`).
* Snapshots: `SnapshotStore` (`include/kernel/snapshot_store.hpp`/`src/snapshot_store.cpp`) persists a full
 `assertions_` snapshot to a single root-level file (`snapshot`, alongside `assertions.log`, not inside
 `indexes/`), written only via the explicit `KnowledgeKernel::write_snapshot()` call — there is no automatic
 cadence, so commit-path latency is unaffected. Unlike the four framed logs, it is always fully rewritten (via
 `write_file_atomically`, like `indexes/checkpoint`) rather than appended to, so it uses one CRC over the whole
 payload instead of per-record CRCs, and `read()` never throws (same "optimization hint, never authoritative"
 philosophy as `IndexCheckpoint`). Crucially, this does **not** shrink `assertions.log` or enable
 truncation/compaction — `assertions_` never shrinks, since audit/timeline queries need full history forever;
 the snapshot only turns "re-parse every record in `assertions.log` on every startup" into "one bulk snapshot
 load plus only the tail committed since the snapshot." `AssertionLog` gained `read_after(AssertionId)` (seeks
 directly to the deterministic byte offset for an id, exploiting the existing fixed-frame-size/no-gap-ids
 invariant already relied on via `assertions_[id - 1]`) and `record_count_hint()` (an O(1) file-size estimate,
 used only to sanity-check a snapshot isn't claiming to cover more records than the log could contain).
 `KnowledgeKernel`'s constructor only uses a usable snapshot on the same fast path already used by the index
 checkpoint (indexes loaded cleanly and checkpoint matches); the full-rebuild fallback path ignores the
 snapshot entirely, since it must `apply()` every record from ID 1 to rebuild `IndexManager` regardless. See
 `docs/storage_format.md`'s "Snapshot" section. Covered by `tests/snapshot_store_tests.cpp` (round trip, missing
 file, corrupt magic/version/crc/record-count-mismatch all return `nullopt` without throwing, overwrite
 replaces prior snapshot), additions to `tests/assertion_log_tests.cpp` for `read_after`/`record_count_hint`,
 and additions to `tests/knowledge_kernel_tests.cpp`
 (`write_snapshot_then_restart_uses_snapshot_and_replays_only_the_tail`,
 `stale_or_corrupt_snapshot_falls_back_to_full_replay`, `snapshot_ahead_of_log_is_ignored`).
* Segment files: `AssertionLog` no longer stores assertions in a single `assertions.log` file. It
 manages a directory of fixed-capacity segment files (`segments/0000000000.seg`,
 `segments/0000000001.seg`, ...; `StorageConfig::segment_directory()`/`segment_path(size_t)`), sized by
 `StorageConfig::max_records_per_segment` (default 100,000 — a storage-layout placeholder, not a tuned
 performance number). Scope is `AssertionLog` only, per explicit user confirmation — the three index logs
 stay single-file, since they're derived/rebuildable and already self-heal via `overwrite_all`. Segment
 index `k` deterministically holds ids `[k*max_records_per_segment + 1, (k+1)*max_records_per_segment]`;
 this is exact, not a hint, because `append()` checks capacity before writing, so a segment is only ever
 rolled from after its previous record was already fully appended and fsynced in an earlier call —
 meaning every non-active segment is guaranteed exactly `max_records_per_segment` complete records, and
 only the single active (highest-index) segment can ever be short or have a torn trailing frame. This
 lets `read_after`/`record_count_hint` skip or size whole historical segments via pure index arithmetic,
 with no manifest/segment-metadata file. The shared per-record read loop's tail-tolerance now takes a
 `tolerate_trailing_anomaly` flag, true only for the active/last segment on disk — the same anomaly in an
 earlier, already-rolled-from segment always throws, since it's proven impossible under normal operation
 there. `AssertionLog`'s constructor seeds its active-segment record count from a file-size estimate (not
 a full parse), matching `record_count_hint()`'s existing "never throw during construction" philosophy —
 a full parse there was tried first but rejected because it made corrupted active-segment content throw
 during construction instead of lazily on `read_all()`/`read_after()`, breaking existing recovery-test
 expectations. `AssertionLog`'s public interface (`append`/`read_all`/`read_after`/`record_count_hint`) is
 unchanged, so `StorageEngine`/`KnowledgeKernel`/the snapshot feature needed no changes beyond the
 constructor call site. This is a breaking, non-migrated on-disk format change, same precedent as the
 checksum format change — existing local data directories must be deleted and rebuilt from scratch. See
 `docs/storage_format.md`'s "Segmented assertion log" section. Covered by additions to
 `tests/assertion_log_tests.cpp` (`assertion_log_rolls_over_to_a_new_segment_when_capacity_is_reached`,
 `assertion_log_read_all_spans_multiple_segments_in_order`,
 `assertion_log_read_after_skips_whole_segments_before_the_seek_point`,
 `assertion_log_read_after_seeks_within_the_straddling_segment`,
 `assertion_log_record_count_hint_spans_multiple_segments`,
 `assertion_log_tail_checksum_mismatch_in_the_active_segment_is_tolerated`,
 `assertion_log_checksum_mismatch_in_a_non_active_segment_throws`), plus the existing single-segment
 corruption/read tests adapted to the directory-based constructor and per-segment file paths.

### Phase 5 — Entity/Predicate Catalog and Payload Store

Future work :

* ✅ Entity/predicate name catalog (label ↔ id)
* ✅ Literal value catalog (typed scalar ↔ id)
* ✅ Payload store (large content ↔ id; see implementation status below)
* ✅ Catalog/payload persistence and replay on startup
* ✅ Public `KnowledgeKernel` API for interning and resolving names, values, and payloads

Assertions currently carry only opaque `EntityId`/`PredicateId` values. Nothing in the kernel persists what those
ids mean, so every one of the "Current North Star" queries ("What do we currently know about Alice?") is
unanswerable through the public API without an out-of-band mapping the caller has to invent and maintain
themselves. Nor is every subject or object a *named* thing in the first place — some are plain values (a number, a
sentence) and some are large content (a whole document), neither of which behaves like "Alice" or "Acme Corp." This
phase closes both gaps without introducing a query language, ontology, or graph traversal — those stay out of
scope per "Do Not Do Yet".

Design :

* `Assertion` and its on-disk format are unchanged by this phase: `subject`/`predicate`/`object` remain
 `EntityId`/`PredicateId` exactly as today. This phase is purely additive — no breaking format change, unlike the
 Phase 4 segment/checksum work.
* A `Catalog` component (naming follows `IndexManager`'s precedent: short, explicit) handles two kinds of
 interning, both idempotent/content-addressed — the same input always resolves to the same id :
  * Names : short human-assigned labels for entities and predicates ("Alice", "works_at", "Acme Corp").
  * Values : typed scalar literals that need identity but aren't named by a person — integers, floats, booleans,
    timestamps, short strings/sentences.
* Large content (whole documents, arbitrary blobs) does not fit the Catalog's small fixed-record framed-log format
 and is not deduplicated by content in this phase. A `PayloadStore` holds arbitrary-size byte content addressed
 directly by `EntityId` : the entity for a document is minted first (an id allocation, same mechanism as
 name/value interning), then its bytes are written to `PayloadStore` keyed by that id. Payloads have no reverse
 "content -> id" lookup, since document content isn't compared for equality in this phase.
* Both the Catalog and the PayloadStore are **authoritative**, not derived/rebuildable indexes like the Phase 3
 indexes — the assertion log only ever stores ids, so nothing else in the system can reconstruct what an id
 means. Both therefore follow `AssertionLog`'s corruption policy (non-tail corruption is fatal) rather than the
 tail-tolerant self-heal behavior used for the Phase 3/4 index logs.
* Storage layout : `catalog/entities.log` and `catalog/predicates.log` (framed, small fixed-ish records — id plus
 a value-kind discriminator plus an inline scalar or short string), and a `payloads/` area for content that
 doesn't fit a fixed-size record (one file per payload, or an append-only log with offsets — an implementation
 detail to settle when this phase starts).
* `StorageEngine` owns the catalog logs and the payload store, exposing `append_entity_mapping`/`load_entities`
 and the predicate equivalent, plus `write_payload(EntityId, content)`/`read_payload(EntityId)`, matching the
 existing `append_*`/`load_*` pattern used for the Phase 3 indexes.
* `KnowledgeKernel` owns the in-memory `Catalog`, replays it (and payload metadata) at startup independently of
 assertion-log replay, and exposes it publicly, e.g. :

 ```cpp
 EntityId intern_entity(std::string_view name);
 PredicateId intern_predicate(std::string_view name);
 EntityId intern_value(Value value); // Value = variant<int64_t, double, bool, Timestamp, std::string>
 EntityId intern_document(std::span<const std::byte> content);

 std::optional<EntityId> find_entity(std::string_view name) const;
 std::optional<PredicateId> find_predicate(std::string_view name) const;
 std::optional<std::string> entity_name(EntityId id) const;
 std::optional<std::string> predicate_name(PredicateId id) const;
 std::optional<std::vector<std::byte>> document_content(EntityId id) const;
 ```

* Out of scope for this phase : name-based overloads of `commit`/`commit_superseding`/`commit_retraction`
 (callers intern first, then commit with ids, same as today), content-hash-based payload deduplication,
 renaming or deleting catalog entries, and streaming/partial reads of large payloads. These may become their own
 follow-up once the base catalog and payload store are in place.

Minimum tests to add, following the existing per-log pattern (round trip, missing file, invalid record size,
incomplete trailing record) plus :

* `interning_the_same_name_twice_returns_the_same_id`
* `interning_the_same_value_twice_returns_the_same_id`
* `unknown_name_lookup_returns_nullopt`
* `catalog_is_preserved_across_kernel_restarts`
* `corrupt_catalog_is_fatal_on_startup` (documents the deliberate divergence from the Phase 3 indexes' self-heal
 behavior)
* `payload_round_trips_large_content`
* `payload_is_preserved_across_kernel_restarts`
* `corrupt_payload_is_fatal_on_startup`

Current implementation status:

* `Value` (`include/kernel/value.hpp`, header-only) is a tagged struct, not `std::variant`, with
 `ValueKind` in `{Text, Int64, Double, Bool, Timestamp}`, defaulted C++20 member-wise `operator==`,
 and a `std::hash<knk::Value>` specialization so it can be used directly as an `unordered_map` key.
* `EntityCatalogLog` (`include/kernel/entity_catalog_log.hpp`/`src/entity_catalog_log.cpp`) persists
 `{EntityId, Value}` to `catalog/entities.log`; `PredicateCatalogLog`
 (`include/kernel/predicate_catalog_log.hpp`/`src/predicate_catalog_log.cpp`) persists
 `{PredicateId, name}` to `catalog/predicates.log`. Both reuse the shared `KNK1` header +
 `[record_size][payload][crc32]` framing and `crc32`/`fsync_file` from the existing logs, but are
 the first **variable-length** record payloads in the codebase — see `docs/storage_format.md`'s new
 "Entity/predicate catalog" section for the exact byte layout and the one new corruption category
 (a length prefix inconsistent with the remaining payload bytes, which always throws, no tail
 tolerance). Neither log has `overwrite_all`: both are authoritative, like `AssertionLog`, so
 non-tail corruption is always a thrown `std::runtime_error`, never self-healed.
* `Catalog` (`include/kernel/catalog.hpp`/`src/catalog.cpp`) is the in-memory counterpart —
 `add_entity`/`add_predicate` are the single mutation entry point used identically for a fresh
 interning commit and for full replay at startup (there is no `IndexManager`-style dual
 add/restore path, since Catalog has exactly one source of truth: its own log files). Each advances
 `next_entity_id_`/`next_predicate_id_` to `max(next_*, id + 1)`, mirroring how `next_id_` is
 restored by replaying `assertions.log`.
* `KnowledgeKernel` gained a `Catalog catalog_` member and public
 `intern_entity`/`intern_value`/`intern_predicate` (durable-before-visible: append to the
 corresponding catalog log, then `catalog_.add_*`, exactly like `commit`'s
 append-then-apply ordering) plus `find_entity`/`find_value`/`find_predicate`/`entity_name`/
 `entity_value`/`predicate_name` lookups. `intern_entity(name)` is sugar for
 `intern_value(Value::of_text(name))`; `entity_name(id)` returns `nullopt` unless the interned
 `Value` is `Text`.
* Catalog replay in `KnowledgeKernel`'s constructor is a small, unconditional, **uncaught** block
 (reads both catalog logs via `StorageEngine::load_entity_catalog`/`load_predicate_catalog` and
 calls `catalog_.add_entity`/`add_predicate`) placed before the existing
 snapshot/checkpoint/tail-vs-full-replay branching — deliberately outside that branching's
 try/catch-and-self-heal machinery, since Catalog has no relationship to `assertions.log` to fall
 back to.
* `StorageConfig` gained `catalog_directory()` (`root/catalog`), `entity_catalog_path()`
 (`catalog/entities.log`), and `predicate_catalog_path()` (`catalog/predicates.log`).
 `StorageEngine` owns both catalog logs and exposes matching `append_entity_catalog_entry`/
 `load_entity_catalog` and `append_predicate_catalog_entry`/`load_predicate_catalog` (no
 `rewrite_*`, consistent with there being no self-heal).
* Accepted limitation, documented rather than solved: Catalog-minted `EntityId`s share the same id
 space as caller-chosen `EntityId`s (e.g. `examples/knowledge_kernel_demo.cpp`'s
 `EntityId external_feed = 9000;`). Avoiding
 collisions between the two is the caller's responsibility, same as it already is for all
 `EntityId` usage today.
* Covered by `tests/entity_catalog_log_tests.cpp` and `tests/predicate_catalog_log_tests.cpp`
 (round trip across all `ValueKind`s for the entity log; missing file; invalid/undersized record
 size; invalid header; partial header; tail checksum mismatch; non-tail checksum mismatch;
 incomplete trailing record; and a catalog-specific malformed-length-prefix test with a
 hand-computed valid checksum), `tests/catalog_tests.cpp` (add/find/next-id behavior for both id
 spaces, plus a test confirming a `Text` value and an `Int64` value with "the same" content resolve
 to different ids), and additions to `tests/knowledge_kernel_tests.cpp`
 (`intern_entity_is_idempotent_and_returns_the_same_id_for_the_same_name`,
 `intern_value_is_idempotent_for_numeric_and_text_values`,
 `find_entity_returns_nullopt_for_an_unknown_name`, `entity_name_resolves_a_previously_interned_name`,
 `predicate_name_resolves_a_previously_interned_predicate`,
 `catalog_is_preserved_across_kernel_restarts`, `corrupt_entity_catalog_is_fatal_on_startup`,
 `corrupt_predicate_catalog_is_fatal_on_startup`).
* `PayloadStore` (`include/kernel/payload_store.hpp`/`src/payload_store.cpp`) persists arbitrary-size
 byte content one-file-per-payload at `payloads/<id>.payload`, with no manifest — `existing_ids()`
 discovers what's on disk by scanning the directory's filenames, the same "derive everything from
 directory contents" approach the segmented assertion log uses. Each file uses a small standalone
 format (`[4-byte magic "KNKD"][uint32 version][uint64 content length][content bytes][uint32 crc32]`,
 crc over content only) and is always written in full via `write_file_atomically`, never appended to
 — like `indexes/checkpoint`/`snapshot`, so a crash mid-write can never leave a torn file visible at
 the real path. Unlike those two hint-only files, though, `read()` always throws on any anomaly in a
 present file rather than degrading gracefully: since a present file is guaranteed fully-formed, any
 anomaly is genuine corruption, and `PayloadStore` is authoritative (see `docs/storage_format.md`'s
 new "Payload store" section).
* `Catalog` gained `allocate_entity_id()` (mints a fresh id from the same counter `add_entity` uses,
 for `intern_document`, but records no name/value mapping — a document has nothing to put in
 `entity_ids_`/`entity_values_`) and `note_allocated_entity_id(id)` (advances the counter past a
 document id discovered by replaying `PayloadStore` at startup, the out-of-band counterpart needed
 because document ids have no `entities.log` record to replay from).
* `KnowledgeKernel` gained `intern_document(std::span<const std::byte>)` (mints via
 `Catalog::allocate_entity_id`, then `storage_.write_payload`) and
 `document_content(EntityId) const` (`storage_.load_payload`, returns `nullopt` only if nothing was
 ever interned for that id). `StorageConfig` gained `payload_path(EntityId)`; `StorageEngine` gained
 `write_payload`/`load_payload`/`existing_payload_ids`, matching the existing `append_*`/`load_*`
 pattern (no `rewrite_*`, consistent with there being no self-heal).
* `KnowledgeKernel`'s constructor extends the uncaught catalog-replay block (same one that loads
 `entities.log`/`predicates.log`) with a loop over `storage_.existing_payload_ids()` that calls
 `storage_.load_payload(id)` for each — reading (not just listing) every payload at startup so
 corruption is caught eagerly, matching `corrupt_payload_is_fatal_on_startup` — and feeds each id
 into `catalog_.note_allocated_entity_id` to restore id-space continuity across restarts.
* Covered by `tests/payload_store_tests.cpp` (round trip incl. empty content, missing id returns
 `nullopt`, write replaces a prior payload for the same id, `existing_ids` lists every written
 payload and is empty when the directory doesn't exist, and three corruption throws: invalid header,
 inconsistent length field, checksum mismatch), additions to `tests/catalog_tests.cpp`
 (`allocate_entity_id_advances_the_counter_without_adding_a_mapping`,
 `note_allocated_entity_id_advances_past_the_given_id_without_adding_a_mapping`), and additions to
 `tests/knowledge_kernel_tests.cpp` (`payload_round_trips_large_content`,
 `intern_document_mints_ids_from_the_shared_entity_id_space`,
 `payload_is_preserved_across_kernel_restarts`, `corrupt_payload_is_fatal_on_startup`).
* Verified on 2026-07-16: `cmake --build build && ctest --test-dir build --output-on-failure`
 passes (14/14 test binaries). `src/main.cpp` was removed; `kernel_demo` now builds from
 `examples/knowledge_kernel_demo.cpp`, a comprehensive walkthrough of commit/query semantics,
 supersession, retraction, audit/timeline history, Catalog interning, PayloadStore documents, and
 recovery across a restart, run and manually inspected end to end.

### Phase 6 — Provenance & Agentic Interface

Future work :

* ✅ Provenance log (`AssertionId` -> source `EntityId` + method, "which source produced this claim?";
 see implementation status below)
* `explain(AssertionId)` — walk the supersession/retraction chain to its root
* Conflict detection (`find_conflicts`) — overlapping active assertions for the same subject/predicate
* `KernelCommand`/`KernelResult` — a closed, serializable command layer over the existing public API

This phase gives everything else in the roadmap two things it depends on: a durable notion of *who or
what asserted this and why*, and a stable, serializable way for an agent to call into the kernel
without linking against the raw C++ API. Neither requirement is new — both are direct gaps against the
"Current North Star" below (no way to answer "which source produced this claim?" or "which assertions
conflict?" today) — but closing them is prerequisite groundwork for Phase 7 and Phase 8, which is why
this phase comes first.

Design :

* `ProvenanceLog` (`include/kernel/provenance_log.hpp`/`src/provenance_log.cpp`) persists
 `{AssertionId, EntityId source, Timestamp recorded_at, std::string method}` to
 `provenance/provenance.log`, reusing the shared `KNK1` header + `[record_size][payload][crc32]`
 framing (variable-length payload, same precedent as `EntityCatalogLog`'s `Text` case). Deliberately
 a side-log keyed by `AssertionId`, not a new field on `Assertion` — this keeps the raw-struct
 on-disk format untouched (no second breaking format change, unlike the Phase 4 segment/checksum
 work). Authoritative like the catalog logs: nothing in `assertions.log` encodes this, so non-tail
 corruption is fatal on startup, with no `overwrite_all`/self-heal.
* `source` is just an `EntityId` — a person, an ingestion pipeline, an agent, or a predictor model
 (Phase 8) is interned into `Catalog` exactly like any other entity, so resolving "which source
 produced this claim" reuses `entity_name`/`entity_value` for free instead of inventing a second
 identity system.
* `KnowledgeKernel` gains `record_provenance(AssertionId, EntityId source, std::string method)`,
 `provenance_for(AssertionId) -> optional<ProvenanceRecord>`, and `explain(AssertionId) ->
 vector<Assertion>` (walks `supersedes_id`/`retracts_id` back to the root assertion, resolving each
 hop's provenance) — this is the concrete answer to "why does the kernel believe this."
* `find_conflicts(EntityId subject, PredicateId predicate) -> vector<std::pair<Assertion, Assertion>>`
 is pure read-side logic over the existing `assertions_for_subject`/current-index lookups — any two
 `Active`, time-overlapping assertions for the same subject/predicate with a different object. No new
 storage. This is also the detection primitive Phase 7's entity-merge tooling is expected to consume.
* `KernelCommand`/`KernelResult` (`include/kernel/kernel_command.hpp`/`kernel_result.hpp`): a closed
 `std::variant` covering every existing public `KnowledgeKernel` method (commit/commit_superseding/
 commit_retraction, each query method, intern_entity/predicate/value/document, find_conflicts,
 explain, ...), plus `KnowledgeKernel::execute(const KernelCommand&) -> KernelResult`, a thin 1:1
 dispatch switch, not new business logic. This is explicitly not a query language: every command
 mirrors an existing method exactly, reified as data so a boundary (in-process today; MCP/HTTP/gRPC
 could wrap it later without redesigning the kernel) can serialize a call instead of doing direct
 method dispatch. No network transport is added in this phase.

Minimum tests to add, following the existing per-log pattern (round trip, missing file, invalid record
size, incomplete trailing record) plus :

* `provenance_is_recorded_and_resolves_to_a_source_entity`
* `explain_walks_the_supersession_chain_to_its_root`
* `find_conflicts_detects_overlapping_active_assertions_for_the_same_subject_predicate`
* `provenance_is_preserved_across_kernel_restarts`
* `corrupt_provenance_log_is_fatal_on_startup`
* one round-trip test per `KernelCommand` variant, confirming `execute()` returns the same result as
 calling the mirrored method directly

Current implementation status :

* `ProvenanceLog` (`include/kernel/provenance_log.hpp`/`src/provenance_log.cpp`) persists
 `{AssertionId assertion_id, EntityId source, Timestamp recorded_at, std::string method}` to
 `provenance/provenance.log`, reusing the shared `KNK1` header + `[record_size][payload][crc32]`
 framing with a variable-length payload (same precedent as `PredicateCatalogLog`'s `Text` case; see
 `docs/storage_format.md`'s new "Provenance log" section for the exact byte layout). Authoritative
 like the catalog logs: no `overwrite_all`/self-heal, and non-tail corruption is a fatal thrown
 `std::runtime_error`.
* `StorageConfig` gained `provenance_directory()` (`root/provenance`) and `provenance_log_path()`
 (`provenance/provenance.log`). `StorageEngine` owns the log and exposes
 `append_provenance_entry(assertion_id, source, recorded_at, method)`/`load_provenance()` (no
 `rewrite_*`, consistent with there being no self-heal).
* `KnowledgeKernel` gained a `std::unordered_map<AssertionId, ProvenanceRecord> provenance_` member,
 `record_provenance(AssertionId, EntityId source, Timestamp recorded_at, std::string method)`
 (durable-before-visible: validate the target assertion exists, then append to the log, then update
 the map), and `provenance_for(AssertionId) -> optional<ProvenanceRecord>`. Replay lives in the same
 uncaught, authoritative constructor block that loads the catalog logs and payloads, before the
 snapshot/checkpoint branching; the log is append-only, so replaying in commit order and overwriting
 leaves the last-recorded provenance per assertion winning.
* **Deliberate divergence from the design sketch above:** `record_provenance` takes a caller-supplied
 `recorded_at` `Timestamp` rather than reading a wall clock internally, matching how `observed_at` is
 supplied to `commit` everywhere in the kernel (no code path anywhere reads a clock, keeping replay
 deterministic). The rest of the sketch (`source` as an `EntityId`, side-log keyed by `AssertionId`,
 authoritative corruption policy) is implemented as written.
* `explain`, `find_conflicts`, and the `KernelCommand`/`KernelResult` layer are **not yet
 implemented** — this status block covers only the provenance-log groundwork.
* Covered by `tests/provenance_log_tests.cpp` (append/read round trip incl. empty method; missing
 file; invalid/undersized record size; invalid header; partial header; tail checksum mismatch;
 non-tail checksum mismatch; incomplete trailing record; malformed method-length prefix with a
 hand-computed valid checksum) and additions to `tests/knowledge_kernel_tests.cpp`
 (`provenance_is_recorded_and_resolves_to_a_source_entity`,
 `record_provenance_rejects_an_unknown_assertion_target`,
 `provenance_is_preserved_across_kernel_restarts`, `corrupt_provenance_log_is_fatal_on_startup`).
* Verified on 2026-07-18: `cmake --build build && ctest --test-dir build --output-on-failure` passes
 (15/15 test binaries).

### Phase 7 — Self-Improvement: Merge & Prune

Future work :

* Entity merge (`merge_entities`) — one-way, append-only redirect for deduplicating entities
* `Catalog::resolve(EntityId)` — transitive redirect resolution at the query boundary
* Segment archival (`archive_segments_before`) — compaction, not deletion

Design :

* `merge_entities(EntityId keep, EntityId absorb)` is backed by a new durable, append-only
 `EntityMergeLog` (`catalog/entity_merges.log`, same framing and authoritative-corruption treatment
 as the other catalog logs: `{EntityId absorbed, EntityId surviving, Timestamp merged_at}`).
 `Catalog` gains `resolve(EntityId) -> EntityId`, following redirects transitively (merging A into B,
 then B into C, makes `resolve(A) == C`). Every query-path method that takes a caller-supplied
 `EntityId` resolves through it first. Per an explicit design decision, this is one-way and
 forward-only: a merge that turns out wrong is corrected by a *new* merge/correction recorded going
 forward, never by mutating or reversing the original redirect record — consistent with the
 append-only philosophy the rest of the kernel already follows.
* **Assertions are never rewritten by a merge.** `assertions_` keeps the original subject/object ids
 exactly as committed; `resolve()` is applied only at the query boundary. This mirrors how
 `IndexManager` never mutates historical index entries, only adds tombstones.
* Assertion-level consolidation (combining two corroborating assertions into one) is deliberately
 *not* a new primitive: it is expressed as the existing `commit_superseding` with a synthesized,
 combined confidence value. Documented explicitly so this phase doesn't grow a second, competing
 correction mechanism alongside supersession.
* **Pruning means compaction/archival, never deletion.** This extends the existing Phase 4 segmented
 `AssertionLog` design rather than replacing it: `archive_segments_before(AssertionId)` moves
 already-rolled-from segments (guaranteed complete and immutable once rolled, per the existing
 segment invariant) into `segments/archive/` — still fully readable by `read_all`/`read_after`, just
 not paged into the hot working set by default. This does not shrink queryable history: audit and
 timeline queries still see archived segments. True, irreversible deletion is an explicit **non-goal**
 of this phase, not deferred future work — the "log is source of truth, never shrinks" principle
 stays intact, and any future request for real erasure (e.g. for compliance) needs its own explicit
 decision, not a quiet extension of pruning.

Minimum tests to add :

* `resolve_follows_a_merge_redirect`
* `resolve_collapses_transitive_merge_chains`
* `resolve_is_identity_for_an_unmerged_id`
* `merge_entities_makes_queries_for_the_absorbed_id_resolve_to_the_surviving_id`
* `merged_entity_redirect_is_preserved_across_kernel_restarts`
* `assertions_are_not_rewritten_by_a_merge`
* `archive_segments_before_moves_only_fully_rolled_segments`
* `archived_segments_remain_readable_via_read_all`

### Phase 8 — Anticipatory Layer: Prediction & Causal Hypotheses

Future work :

* `AssertionStatus::Hypothesis` — a labeled, provenance-required status for machine-suggested facts
* `commit_hypothesis` / `hypotheses_for` — writing and listing open predictions
* Bounded local graph traversal (`neighbors`, `co_occurring_predicates`) — feature extraction for
 external prediction/causal-inference tooling, not a query language

Per an explicit design decision, the kernel stays a *substrate* for this phase: it stores and clearly
labels machine-suggested knowledge and exposes just enough bounded read access for an external
predictor to do its actual modeling, but the prediction/causal-inference computation itself
(embeddings, graph neural nets, Granger-causality-style temporal inference, or anything else with real
statistical machinery) lives outside the kernel, in whatever process is acting as the agent. This keeps
the kernel small and correctness-first rather than turning it into an ML system.

Design :

* `AssertionStatus` gains a `Hypothesis` value. This is purely additive to the enum (same underlying
 size), so — unlike adding a new field to `Assertion` would — it does **not** break the on-disk
 raw-struct format. Hypothesis-status records are excluded from `current`/`valid_at`/`known_at`/
 `valid_at_known_at` by default, the same treatment `Superseded`/`Retracted`/`Retraction` already get.
* `commit_hypothesis(...)` mirrors `commit`'s signature, tags status `Hypothesis`, and *requires* a
 source `EntityId` parameter (internally calling Phase 6's `record_provenance`) — an unsourced
 hypothesis is a contradiction in terms for this design, so there is no optional-provenance path for
 hypotheses the way there is for ordinary commits.
* Promotion is not a new primitive either: a confirmed hypothesis is promoted via ordinary `commit` or
 `commit_superseding`. The original hypothesis record is left untouched in `commit_history`, which
 gives a free audit trail of "the kernel predicted X, and X was later confirmed or rejected" without
 any new storage.
* `hypotheses_for(EntityId subject) -> vector<Assertion>` mirrors `current` but selects
 Hypothesis-status records.
* `neighbors(EntityId subject, size_t max_hops = 1)` and `co_occurring_predicates(EntityId subject)`
 are built entirely from existing `IndexManager`/`assertions_for_subject` primitives — no new
 storage. Deliberately capped at a small, fixed hop count with no path queries and no joins: this is
 feature extraction for an external model, not a general graph query language, and the cap is what
 keeps it from becoming the "complex graph traversal" the Do Not Do Yet list otherwise forbids (see
 below).
* No new storage for causal reasoning either: a causal hypothesis is just
 `commit_hypothesis(cause, may_cause_predicate, effect, ...)`, discoverable like any other hypothesis.
 The already-public `observed_time_timeline`/`valid_time_timeline` are what an external
 causal-inference tool consumes to *produce* that hypothesis in the first place — this phase adds the
 vocabulary for writing the result back in a labeled, queryable, confirmable/retractable way, not a
 causal inference algorithm.

This phase requires narrowing (not removing) two "Do Not Do Yet" items — see that section below for
the exact wording.

Minimum tests to add :

* `commit_hypothesis_is_excluded_from_current_and_valid_at`
* `hypotheses_for_returns_open_predictions`
* `promoting_a_hypothesis_preserves_it_in_commit_history`
* `neighbors_respects_max_hops`
* `commit_hypothesis_requires_a_source_and_records_provenance`

### Phase 9 — Performance

Future work :

* SIMD scanning
* Memory - mapped segments
* Lock - free readers
* NUMA - aware allocator
* Background compaction
* Bloom filters
* Compression

Do not introduce advanced performance features before the correctness model is stable.

-- -

## Architectural Principles

### 1. The log is the source of truth

The assertion log records committed knowledge changes.

In - memory vectors, indexes, and caches are derived state.

Do not make indexes authoritative.

There is a second, recognized category distinct from both: **authoritative metadata logs**
(`Catalog`'s `EntityCatalogLog`/`PredicateCatalogLog`, Phase 6's `ProvenanceLog`, Phase 7's
`EntityMergeLog`). These are not derived from `assertions.log` — it never stores names, values,
provenance, or merge decisions, so there is nothing to rebuild them from — but they are also not
indexes over assertion data. They get `AssertionLog`'s corruption treatment (non-tail corruption is
fatal, no self-heal) rather than the Phase 3 indexes' catch-and-rebuild treatment, precisely because
each is its own source of truth for the metadata it holds.

### 2. Append-only first

Prefer append - only writes.

Do not mutate records on disk in place.

In - memory state may mark old assertions as superseded or retracted during replay, but the durable log should preserve history.

### 3. Durable-before-visible

A committed assertion must be persisted before it becomes visible in memory.

Correct order :

```cpp
storage_.append_assertion(assertion);
apply_assertion(assertion);
```

Incorrect order:

```cpp
apply_assertion(assertion);
storage_.append_assertion(assertion);
```

### 4. Replay and commit are different

Replay:

```text
read log
→ apply to memory only
```

Commit:

```text
write to log
→ apply to memory
```

Replay must never append back to the log.

### 5. Keep layers separate

Expected layering:

```text
KnowledgeKernel
Query semantics and public API

IndexManager
In - memory and persistent indexes

StorageEngine
Durable storage coordination

AssertionLog
Append / read raw assertion records
```

Do not let low - level storage classes understand query semantics.

Do not let query code directly manage files.

-- -

## Recommended Repository Structure

```text
knowledge - kernel /
├── CMakeLists.txt
├── AGENTS.md
├── README.md
├── include /
│   └── kernel /
│       ├── assertion.hpp
│       ├── assertion_log.hpp
│       ├── index_manager.hpp
│       ├── knowledge_kernel.hpp
│       ├── storage_config.hpp
│       └── storage_engine.hpp
├── src /
│   ├── assertion_log.cpp
│   ├── index_manager.cpp
│   ├── knowledge_kernel.cpp
│   └── storage_engine.cpp
├── tests /
│   ├── assertion_log_tests.cpp
│   ├── index_manager_tests.cpp
│   ├── knowledge_kernel_tests.cpp
│   └── recovery_tests.cpp
├── examples /
│   └── basic_usage.cpp
├── benchmarks /
│   └── commit_benchmark.cpp
└── docs /
├── architecture.md
├── data_model.md
├── query_semantics.md
└── storage_format.md
```

Namespace should be:

```cpp
namespace knk {
}
```

Prefer short, explicit names:

```cpp
knk::Assertion
knk::KnowledgeKernel
knk::StorageEngine
knk::IndexManager
```

-- -

## Core Types

Use strong, explicit type aliases initially:

```cpp
using EntityId = uint64_t;
using PredicateId = uint64_t;
using AssertionId = uint64_t;
using Timestamp = int64_t;
```

Use Unix seconds for `Timestamp` during early development.

Use :

```cpp
constexpr Timestamp OPEN_ENDED = 0;
```

to represent an open - ended validity interval.

-- -

## Assertion Semantics

An assertion is a versioned temporal claim.

Recommended structure:

```cpp
struct Assertion {
AssertionId id;

EntityId subject;
PredicateId predicate;
EntityId object;

Timestamp valid_from;
Timestamp valid_to;
Timestamp observed_at;

double confidence;

AssertionStatus status;

AssertionId supersedes_id;
AssertionId retracts_id;
};
```

Recommended status enum :

```cpp
enum class AssertionStatus {
Active,
Superseded,
Retracted,
Retraction
};
```

### Active

The assertion is currently accepted by the kernel.

### Superseded

The assertion was once accepted but has been replaced by a better assertion.

Example:

```text
#1 Alice works_at Acme, valid_to = OPEN
#2 Alice works_at Acme, valid_to = 2024-07-01, supersedes #1
```

During replay, `#1` becomes superseded and `#2` becomes active.

### Retracted

The assertion should no longer be treated as knowledge.

Example:

```text
#1 Alice born_in Paris
#2 retracts #1
```

A retracted assertion remains in audit history but is excluded from normal query results.

### Retraction

The assertion-like audit record that records the retraction event.

Example:

```text
#1 Alice born_in Paris
#2 retraction record, retracts #1
```

During replay, `#1` becomes retracted and `#2` remains a `Retraction` record. Retraction records are preserved in memory and the subject/audit index so `get(id)` and future timeline/audit queries can explain how knowledge changed. They are excluded from current, valid-time, observed-time, and valid-at-known-at query results because they are not active facts about the world.

-- -

## Supersession and Retraction

In the current simplified model, a correction may be represented as a new assertion with `supersedes_id`.

A pure retraction is currently represented as an assertion - like log record with `status == AssertionStatus::Retraction` and `retracts_id` set, but agents should be careful: long term, the project may introduce a separate `KnowledgeEvent` type.

For now:

```cpp
if (assertion.supersedes_id != 0) {
	mark_superseded(assertion.supersedes_id);
	assertions_.push_back(assertion);
}

if (assertion.retracts_id != 0) {
	mark_retracted(assertion.retracts_id);
	assertions_.push_back(assertion); // Retraction audit record, not active knowledge.
}
```

Public commit APIs must validate supersession and retraction targets before appending to the durable log. Failed supersession/retraction attempts must not persist invalid records or burn assertion IDs.

Before changing this behavior, update tests and document the decision.

-- -

## Query Semantics

Normal queries should exclude:

```text
Superseded
Retracted
Retraction
```

unless explicitly querying history or audit state.

Core query methods:

```cpp
std::optional<Assertion> get(AssertionId id) const;

std::vector<Assertion> current(EntityId subject) const;

std::vector<Assertion> valid_at(
    EntityId subject,
    Timestamp valid_time
) const;

std::vector<Assertion> known_at(
    EntityId subject,
    Timestamp observed_time
) const;

std::vector<Assertion> valid_at_known_at(
    EntityId subject,
    Timestamp valid_time,
    Timestamp observed_time
) const;

std::vector<Assertion> timeline(
    EntityId subject,
    PredicateId predicate
) const;
```

Interval semantics:

```text
valid_from <= t < valid_to
```

For open - ended intervals:

```text
valid_to == OPEN_ENDED
```

means the assertion remains valid until closed, superseded, or retracted.

-- -

## Storage Semantics

### AssertionLog

The assertion log should do only this:

```text
append assertion records
read assertion records
```

It should not:

* Rebuild indexes.
* Interpret bitemporal semantics.
* Decide which assertion is current.
* Resolve supersession.
* Read environment variables.

### StorageEngine

The storage engine coordinates durable storage.

It may own:

```text
AssertionLog
PayloadStore
SegmentManager
SnapshotManager
```

Current responsibilities:

```cpp
append_assertion(const Assertion& assertion);
load_assertions() const;
```

### StorageConfig

Configuration should be injected from the application boundary.

Do not call `std::getenv()` inside storage components.

Recommended:

```cpp
struct StorageConfig {
	std::filesystem::path root;

	std::filesystem::path assertion_log_path() const {
		return root / "assertions.log";
	}

	std::filesystem::path index_directory() const {
		return root / "indexes";
	}

	std::filesystem::path payload_directory() const {
		return root / "payloads";
	}
};
```

-- -

## Recovery Rules

On startup:

```cpp
KnowledgeKernel::KnowledgeKernel(StorageConfig config)
	: storage_(config) {
	auto records = storage_.load_assertions();

	for (const auto& record : records) {
		apply_replayed_assertion(record);
	}
}
```

Replay must:

* Restore `next_id_`.
* Rebuild `assertions_`.
* Rebuild all in - memory indexes.
* Apply supersession and retraction.
* Ignore or safely handle incomplete trailing records if the log supports that policy.
* Fail loudly on structural corruption unless a recovery policy exists.

-- -

## IndexManager

The `IndexManager` should own indexing details.

Initial indexes:

```text
subject_index
current_index
```

Phase 3 indexes:

```text
subject index
predicate index
current - state index
observed - time index
```

IndexManager should expose methods like:

```cpp
void add(const Assertion& assertion);
void mark_superseded(AssertionId id);
void mark_retracted(AssertionId id);

std::vector<AssertionId> facts_for_subject(EntityId subject) const;
std::vector<AssertionId> current_for_subject(EntityId subject) const;
std::vector<AssertionId> observed_before(Timestamp t) const;
```

Do not return full assertions from indexes. Return IDs. The kernel should resolve IDs into assertions.

              -- -

## Testing Requirements

Every semantic change must have tests.

Use simple standard - library tests initially unless the project has adopted a test framework.

Minimum required tests:

```text
✅ commit_and_get_assertion
✅ failed_commit_does_not_burn_id
✅ current_assertion_returns_open_ended_assertion
✅ valid_at_returns_historical_assertion
✅ valid_at_respects_exclusive_valid_to
✅ known_at_excludes_future_observed_fact
✅ valid_at_known_at_respects_both_times
✅ valid_time_timeline_returns_active_assertions_sorted_by_valid_from
✅ observed_time_timeline_returns_active_assertions_sorted_by_observed_at
✅ commit_history_returns_recorded_assertions_sorted_by_commit_order
✅ assertions_for_subject_returns_all_subject_assertions
✅ assertion_log_appends_and_reads_assertions
✅ assertion_log_returns_empty_when_missing
✅ current_assertion_is_preserved_across_kernels
✅ valid_at_is_preserved_across_kernels
✅ constructor_replays_assertions_and_continues_ids
✅ replay_does_not_append_to_log
✅ superseded_assertion_is_excluded_from_current_queries
✅ retracted_assertion_is_excluded_from_current_queries
✅ recovery_preserves_superseded_state
✅ recovery_preserves_retracted_state
✅ conflicting_active_assertions_can_coexist
✅ IndexManager add/query behavior tests
✅ IndexManager superseded/retracted behavior tests
```

Tests should use temporary directories and clean up after themselves.

Do not use fixed paths like:

```text
. / data
/ tmp / kernel
```

inside tests unless isolated per test.

-- -

## C++ Style

Use C++20.

Prefer:

```cpp
std::filesystem
std::optional
std::vector
std::unordered_map
std::string_view
```

Avoid unnecessary dependencies during Phases 1 and 2.

Prefer clear code over clever code.

Do not introduce templates, custom allocators, memory mapping, SIMD, or lock - free structures before Phase 9.

Use explicit names:

```cpp
apply_replayed_assertion
append_assertion
mark_superseded
mark_retracted
rebuild_indexes
```

Avoid vague names:

```cpp
process
handle
update
do_work
```

-- -

## Error Handling

For now:

* Throw `std::runtime_error` for unrecoverable storage errors.
* Return `std::optional` for missing assertions.
* Return empty vectors for valid empty query results.

Later phases may introduce typed error handling.

Do not silently ignore :

* Invalid record sizes.
* Corrupt files.
* ID collisions.
* Invalid supersession targets.
* Invalid retraction targets.

Incomplete trailing log records may be ignored only if the behavior is documented and tested.

-- -

## Binary Format Rules

Current binary format may be simple, but agents must not assume it is stable.

If changing on - disk layout:

1. Document the new format in `docs / storage_format.md`.
2. Add a version field or magic header if appropriate.
3. Add read / write tests.
4. Add corruption tests.
5. Avoid breaking old tests silently.

Do not use raw `reinterpret_cast` serialization long term without documenting its limitations:

* Padding
* Endianness
* Compiler ABI
* Enum size
* Struct layout

For early local development it is acceptable, but Phase 4 should replace it with explicit serialization.

-- -

## Performance Rules

Do not optimize before correctness.

Phase 9 performance work must be benchmark - driven.

Before adding an optimization:

1. Add or update a benchmark.
2. Record baseline behavior.
3. Implement optimization.
4. Compare results.
5. Keep correctness tests passing.

Do not add SIMD, mmap, lock - free readers, NUMA allocation, Bloom filters, compression, or background compaction before the architecture supports them cleanly.

-- -

## Concurrency Rules

Current phases should assume:

```text
single writer
many readers later
```

Do not add concurrency prematurely.

When concurrency is introduced:

* Reads must see consistent snapshots.
* Commits must be durable before visible.
* Index updates must be atomic relative to query visibility.
* Replaying the log must be deterministic.

-- -

## Documentation Requirements

Update documentation when changing semantics.

Core docs:

```text
docs / architecture.md
docs / data_model.md
docs / query_semantics.md
docs / storage_format.md
```

Every major design decision should answer:

```text
What is the invariant ?
Why is this design chosen ?
What are the failure modes ?
How is it tested ?
```

-- -

## Do Not Do Yet

Unless explicitly instructed, do not implement :

```text
SQL
HTTP API
LLM extraction
voice ingestion
distributed consensus
persistent B - trees
complex graph traversal
RDF compatibility
ontology reasoning
custom memory allocators
compression
replication
```

Two of these items are narrowed, not lifted, by Phase 8 (see "Current Roadmap" above) :

* **complex graph traversal** stays forbidden as a general capability. Phase 8's `neighbors`/
 `co_occurring_predicates` are the one named, explicitly bounded exception (small fixed hop count, no
 path queries, no joins) — a general graph query language is still out of scope.
* **LLM extraction** has always meant *the kernel performing NLP extraction from raw text*. It does
 not mean *an external agent or LLM writing already-structured assertions through the public API* —
 that has always been in scope; Phase 6's `KernelCommand` layer and Phase 8's `commit_hypothesis` are
 both just structured calls into the existing commit machinery, not extraction happening inside the
 kernel. Written down explicitly here because Phase 8 is where the ambiguity would otherwise bite.

**HTTP API** stays fully out of scope: Phase 6 adds a serializable command layer but deliberately no
network transport. Revisiting this item is its own future decision, not implied by anything in the
current roadmap.

The kernel must be correct and recoverable before it becomes broad.

-- -

## Agent Workflow

When making changes :

1. Read this file.
2. Identify the current roadmap phase.
3. Keep changes small.
4. Preserve public API unless asked to change it.
5. Add or update tests.
6. Run build and tests if possible.
7. Explain what changed.
8. Mention any tradeoffs or unfinished work.

A good pull request should be small enough to understand in one sitting.

-- -

## Current North Star

The Knowledge Kernel should eventually answer questions like :

```text
What do we currently know about Alice ?
What did we know about Alice on 2024 - 01 - 01 ?
What was true on 2024 - 01 - 01 according to what we knew on 2024 - 07 - 02 ?
What changed since yesterday ?
Why does the kernel believe this assertion ?
Which assertions conflict ?
Which assertion superseded this one ?
Which source produced this claim ?
What does the kernel currently predict about Alice ?
What existing evidence supports or contradicts a given hypothesis ?
```

Until Phase 6, "Why does the kernel believe this assertion?", "Which assertions conflict?", and
"Which source produced this claim?" were aspirational — nothing in the kernel could answer them
(no provenance existed at all, and there was no conflict-detection query). Phase 6's `explain`,
`find_conflicts`, and `provenance_for` close that gap. The last two questions above are new, added
for Phase 8's `hypotheses_for`/`commit_history` combination.

Every implementation decision should support this long - term direction.

The kernel is not merely storing facts.

It is preserving the history of how knowledge changes.
