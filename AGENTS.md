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
 follow-up once the base catalog and payload store are in place. (`commit` gained exactly this follow-up —
 `commit_by_name` — once the catalog/MCP layers made the round-trip cost of interning first concrete; see the
 note at the end of "Current implementation status" below. `commit_superseding`/`commit_retraction` remain
 id-only, since by the time a caller has a specific assertion to supersede/retract it already has that
 assertion's ids.)

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
* **Added 2026-07-22, after MCP transport landed:** `KnowledgeKernel::commit_by_name(subject_name,
 predicate_name, object, valid_from, valid_to, observed_at, confidence) -> AssertionId` — the
 name-based `commit` overload flagged as out of scope above, built once it landed. Every fact an
 MCP-speaking agent writes for a brand-new subject/object previously cost three tool calls
 (`intern_entity` x2, then `commit`); this collapses that to one. Pure composition, not new storage
 or a new invariant: `intern_entity(subject_name)`, `intern_predicate(predicate_name)`,
 `intern_value(object)` (each already idempotent/durable-before-visible on its own), then
 `commit(...)` with the resulting ids. `object` is a `Value`, not a second name string — a `Text`
 `Value` is exactly what `intern_entity` would produce, so `Value::of_text("Acme")` names an entity
 the same way `Value::of_int64(2010)` interns a literal; one parameter covers both without a second
 "is this a name or a literal" flag. `KernelCommand`/`KernelResult` gained `CommitByNameCommand`
 (mutating, alongside `CommitCommand`), and `mcp_tools`/`docs/mcp_server.md` gained the matching
 `commit_by_name` tool (39 tools total now, up from 38). Deliberately **not** extended to
 `commit_superseding`/`commit_retraction`/`commit_hypothesis`: those all take a target/source
 `AssertionId` the caller can only have gotten from a prior query, which already hands back full
 `Assertion`s (and therefore their subject/predicate/object ids) — the interning friction this
 solves is specific to a fact's *first* commit. Covered by
 `commit_by_name_interns_names_and_commits`, `commit_by_name_reuses_ids_for_repeated_names`, and
 `commit_by_name_supports_a_literal_object` in `tests/knowledge_kernel_tests.cpp`,
 `commit_by_name_command_round_trips` in `tests/kernel_command_tests.cpp`, and
 `commit_by_name_tool_interns_names_and_commits` (plus the updated 39-name completeness set) in
 `tests/mcp_tools_tests.cpp`. `examples/knowledge_kernel_demo.cpp` gained a "Committing by name"
 section.
* **Added 2026-07-23:** `KnowledgeKernel::current_by_name(subject_name) -> vector<Assertion>` — the
 read-side mirror of `commit_by_name`, closing the same friction on the query side: asking "what do
 we currently know about Alice" through MCP previously cost two tool calls (`find_entity("Alice")`
 then `current(id)`) even though the North Star's own lead example asks for exactly this by name.
 **Deliberate asymmetry with `commit_by_name`:** this looks the name up via `find_entity`, it does
 **not** intern it — a read-only query must never spuriously mint a new entity/id as a side effect of
 asking about a name that was never committed (a typo, or a subject the kernel genuinely doesn't know
 yet), so an unresolved name returns an empty vector, matching `current()`'s own behavior for a
 subject id with no current facts, rather than throwing or auto-creating. Pure composition, like
 `commit_by_name`: `find_entity(subject_name)` then `current(*id)`, no new storage. Scoped narrowly to
 just `current`, the one method the North Star example actually names — deliberately **not** extended
 to `valid_at`/`known_at`/`assertions_for_subject`/the other subject-keyed query methods, the same
 "don't extend beyond a concrete, demonstrated friction point" discipline `commit_by_name` itself
 followed against `commit_superseding`/`commit_retraction`/`commit_hypothesis`. `KernelCommand`/
 `KernelResult` gained `CurrentByNameCommand` (query, alongside `CurrentCommand`; reuses the existing
 `vector<Assertion>` `KernelResult` alternative, no new one needed), and `mcp_tools`/
 `docs/mcp_server.md` gained the matching `current_by_name` tool (40 tools total now, up from 39).
 Covered by `current_by_name_resolves_the_named_subject_and_returns_current_assertions` and
 `current_by_name_returns_empty_for_an_unknown_name` in `tests/knowledge_kernel_tests.cpp`,
 `current_by_name_command_round_trips` in `tests/kernel_command_tests.cpp`, and
 `current_by_name_tool_resolves_the_named_subject` (plus the updated 40-name completeness set) in
 `tests/mcp_tools_tests.cpp`. `examples/knowledge_kernel_demo.cpp`'s "Committing by name" section now
 also demonstrates `current_by_name`.

### Phase 6 — Provenance & Agentic Interface

Future work :

* ✅ Provenance log (`AssertionId` -> source `EntityId` + method, "which source produced this claim?";
 see implementation status below)
* ✅ `explain(AssertionId)` — walk the supersession/retraction chain to its root (see implementation
 status below)
* ✅ Conflict detection (`find_conflicts`) — overlapping active assertions for the same
 subject/predicate (see implementation status below)
* ✅ `KernelCommand`/`KernelResult` — a closed, serializable command layer over the existing public
 API (see implementation status below)

**Phase 6 is complete.** All four deliverables above are implemented, tested, and documented.

This phase gives everything else in the roadmap two things it depends on: a durable notion of *who or
what asserted this and why*, and a stable, serializable way for an agent to call into the kernel
without linking against the raw C++ API. Neither requirement is new — both are direct gaps against the
"Current North Star" below (no way to answer "which source produced this claim?" or "which assertions
conflict?" today) — but closing them is prerequisite groundwork for Phase 7 and Phase 8 (in either
order), which is why this phase comes first.

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
 (Phase 7) is interned into `Catalog` exactly like any other entity, so resolving "which source
 produced this claim" reuses `entity_name`/`entity_value` for free instead of inventing a second
 identity system.
* `KnowledgeKernel` gains `record_provenance(AssertionId, EntityId source, std::string method)`,
 `provenance_for(AssertionId) -> optional<ProvenanceRecord>`, and `explain(AssertionId) ->
 vector<Assertion>` (walks `supersedes_id`/`retracts_id` back to the root assertion, resolving each
 hop's provenance) — this is the concrete answer to "why does the kernel believe this."
* `find_conflicts(EntityId subject, PredicateId predicate) -> vector<std::pair<Assertion, Assertion>>`
 is pure read-side logic over the existing `assertions_for_subject`/current-index lookups — any two
 `Active`, time-overlapping assertions for the same subject/predicate with a different object. No new
 storage. This is also the detection primitive Phase 8's entity-merge tooling is expected to consume.
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
* `KnowledgeKernel::explain(AssertionId) -> vector<Assertion>` walks the supersession/retraction chain
 from the given assertion back to its root, one hop at a time via `supersedes_id`/`retracts_id`
 (mutually exclusive on any record, and always pointing at a smaller/earlier id, so the chain strictly
 decreases and terminates with no cycle guard). Returns the chain newest-first (queried assertion, then
 the one it superseded/retracted, ... , down to the original); an unknown or zero id returns an empty
 vector. Provenance resolution per hop is left to the caller via `provenance_for` (that method lands
 with the provenance-log work; `explain` itself has no dependency on it and returns only the assertion
 chain).
* `KnowledgeKernel::find_conflicts(EntityId subject, PredicateId predicate) ->
 vector<std::pair<Assertion, Assertion>>` is pure read-side over the existing subject index: every
 unordered pair of `Active` assertions for the subject/predicate with a different object whose
 half-open `[valid_from, valid_to)` valid-time intervals overlap (`OPEN_ENDED` = unbounded end). No new
 storage. Superseded/retracted assertions are excluded (already resolved), and same-object assertions
 are never reported (same claim, not a contradiction). Pairs are reported in subject-index/commit order.
* `record_provenance`/`provenance_for`/`ProvenanceLog` and the `KernelCommand`/`KernelResult` layer are
 tracked separately — this status block covers only `explain` and `find_conflicts`.
* Covered by additions to `tests/knowledge_kernel_tests.cpp`
 (`explain_walks_the_supersession_chain_to_its_root`,
 `find_conflicts_detects_overlapping_active_assertions_for_the_same_subject_predicate`,
 `find_conflicts_excludes_non_overlapping_and_resolved_assertions`).
* `KernelCommand` (`include/kernel/kernel_command.hpp`) is a closed `std::variant` of 28 plain-data
 command structs, one per public `KnowledgeKernel` operation (the commit family, all query methods,
 `intern_*`, `record_provenance`, `explain`, `find_conflicts`, the `find_*`/`*_name`/`*_value`
 lookups, `document_content`, `provenance_for`, `write_snapshot`). Each struct holds exactly the
 arguments of the mirrored method. The internal replay hooks `apply`/`mark_superseded`/`mark_retracted`
 are deliberately excluded — they mutate in-memory status without a durable record (bypassing
 durable-before-visible/append-only) and exist only for replay, not as caller operations.
* `KernelResult` (`include/kernel/kernel_result.hpp`) is a closed `std::variant` over the distinct
 return types. Because `AssertionId`/`EntityId`/`PredicateId` are all `uint64_t` aliases, every
 id-returning command collapses to the single `AssertionId` alternative (and the `find_entity`/
 `find_value`/`find_predicate` optionals to `std::optional<AssertionId>`) — a variant cannot hold two
 identical alternatives, and the caller already knows the semantic id kind from the command it issued.
 Void-returning commands (`write_snapshot`, `record_provenance`) yield `std::monostate`.
* `KnowledgeKernel::execute(const KernelCommand&) -> KernelResult` (`src/kernel_command.cpp`) is a thin
 `std::visit` dispatch: one `if constexpr` branch per command type calling the mirrored method 1:1,
 with a dependent-`static_assert` final `else` so adding a command without a branch fails to compile.
 It is non-const (the command set includes mutations) and adds no business logic. This is explicitly
 not a query language and adds no network transport — the commands are plain data a future boundary
 (agent, MCP/HTTP/gRPC) could serialize, which is why they carry no behavior of their own.
* Covered by `tests/kernel_command_tests.cpp`: one round-trip test per command variant (28 total),
 each asserting `execute(cmd)` returns the same result as calling the mirrored method directly —
 query commands compared on one kernel, mutating commands across two identically-seeded twin kernels.
 `examples/knowledge_kernel_demo.cpp` gains a "Calling the kernel through the command layer" section
 issuing a `CommitCommand` and a `CurrentCommand` through `execute`.
* Verified on 2026-07-18: `cmake --build build && ctest --test-dir build --output-on-failure` passes
 (16/16 test binaries).

### Phase 7 — Anticipatory Layer: Prediction & Causal Hypotheses

Reordered ahead of the former Phase 7 (now Phase 8, Self-Improvement: Merge & Prune): this phase is
small (an additive enum value plus a few bounded read methods, no new storage format) and directly
closes the last two "Current North Star" questions ("What does the kernel currently predict about
Alice?", "What existing evidence supports or contradicts a given hypothesis?"), whereas merge/prune is
data-hygiene work that earns its keep once real multi-source usage is generating duplicate entities.
Do the higher North-Star-leverage, lower-cost phase first.

**Phase 7 is complete.** All items below are implemented, tested, documented, and covered by the
`KernelCommand`/`KernelResult` layer.

Future work :

* ✅ `AssertionStatus::Hypothesis` — a labeled, provenance-required status for machine-suggested facts
* ✅ `commit_hypothesis` / `hypotheses_for` — writing and listing open predictions
* ✅ Bounded local graph traversal (`neighbors`, `co_occurring_predicates`) — feature extraction for
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

Current implementation status :

* `AssertionStatus` gained `Hypothesis`, purely additive to the enum (no on-disk format break).
 Every existing query method already gated on `status == AssertionStatus::Active` (explicitly in
 `valid_at`/`known_at`/`valid_at_known_at`/`valid_time_timeline`/`observed_time_timeline`/
 `find_conflicts`, or indirectly via `IndexManager::is_current_assertion` for `current`), so
 `Hypothesis`-status records are excluded from all of them with no code changes to those methods.
 `commit_history`, `explain`, and `get` are status-agnostic and correctly surface hypotheses.
* `KnowledgeKernel::commit_hypothesis(subject, predicate, object, valid_from, valid_to, observed_at,
 confidence, source, recorded_at, method)` mirrors `commit`'s append/index/checkpoint/`apply`
 sequence with `AssertionStatus::Hypothesis`, then unconditionally calls the existing
 `record_provenance` — no duplicated provenance logic, no new storage. Accepted, documented gap: a
 crash between the assertion becoming durable/visible and the `record_provenance` call landing can
 leave a replayed hypothesis with no provenance record; this is the same category of gap the Phase 4
 index checkpoint already accepts for assertion-vs-index durability, not new risk, and gets no new
 cross-log atomicity machinery.
* `KnowledgeKernel::hypotheses_for(EntityId subject) -> vector<Assertion>` reads
 `index_manager_.assertions_for_subject` (the same primitive `valid_at`/`commit_history` already use)
 and filters to `Hypothesis` status. No `IndexManager` changes — hypotheses were never added to
 `current_index_` in the first place, since `is_current_assertion` requires `Active` status.
* Promotion required no new code: `commit_superseding` only validates that its target id exists, not
 the target's status, so promoting a hypothesis via ordinary `commit_superseding` already worked.
 Promotion flips the original record's status to `Superseded`, exactly like ordinary Active-to-
 Superseded promotion — "the original hypothesis record is left untouched" means it is never deleted
 or rewritten (still present in `commit_history`/`explain` with its original fields), not that its
 status field is frozen. Its provenance record (e.g. `method == "predicted_by_model"`) is unaffected
 by promotion and remains resolvable via `provenance_for` after the fact.
* Covered by additions to `tests/knowledge_kernel_tests.cpp`:
 `commit_hypothesis_is_excluded_from_current_and_valid_at`, `hypotheses_for_returns_open_predictions`,
 `promoting_a_hypothesis_preserves_it_in_commit_history` (including that provenance survives
 promotion), `commit_hypothesis_requires_a_source_and_records_provenance`, and
 `hypothesis_is_preserved_across_kernel_restarts`.
* `IndexManager` gained a reverse (object -> subject) companion to the current-state index:
 `object_index_`/`assertion_object_` (mirroring `current_index_`/`assertion_keys_`), populated via a
 new `restore_object_entry(EntityId object, AssertionId id, bool active)` and read via
 `current_assertions_by_object(EntityId object) const`. Only current (`Active`, open-ended) assertions
 are tracked, exactly like the forward current index, and `mark_superseded`/`mark_retracted` remove
 from both indexes together. **Deliberately not persisted to its own log file**, unlike the three
 Phase 3 indexes: it carries no corruption/self-heal/checkpoint machinery of its own. It is always
 rebuilt in memory — via `IndexManager::add` (which now also calls `restore_object_entry`) on the
 live-commit and full-replay paths, and via one dedicated bulk pass over the already-in-memory
 `assertions_` vector on the fast/trusted-index startup path (`restore_assertion`, unlike `apply`,
 never touches `IndexManager`, so this path would otherwise leave the reverse index empty after
 restart). That bulk pass is a linear scan of already-loaded data, not a disk re-read, so it doesn't
 undermine the snapshot/checkpoint optimization the fast path exists for.
* `KnowledgeKernel::neighbors(EntityId subject, size_t max_hops = 1) -> vector<EntityId>` is a
 breadth-first traversal from `subject` using `current(entity)` for outgoing edges and
 `IndexManager::current_assertions_by_object` for incoming edges, so it is bidirectional despite
 `IndexManager` only ever having been keyed by subject before this phase. A `visited` set both
 deduplicates and makes the traversal cycle-safe; results are a flat, deduplicated `vector<EntityId>`
 (no paths), matching the "no path queries" restriction. `max_hops` is not hard-capped in code — the
 `visited` set already bounds the work to the reachable-set size, so a large `max_hops` degrades
 gracefully rather than becoming pathological; callers are expected to keep it small per the design
 note above.
* `KnowledgeKernel::co_occurring_predicates(EntityId subject) -> vector<PredicateId>` is a direct,
 public exposure of `IndexManager::predicates_for_subject` — deliberately per-subject, not a
 cross-subject association join, per the "no joins" restriction in the Do-Not-Do-Yet narrowing below.
* Covered by additions to `tests/index_manager_tests.cpp`
 (`add_indexes_active_open_ended_assertions_as_current_by_object`,
 `mark_superseded_and_mark_retracted_remove_the_object_index_entry`,
 `restore_object_entry_reproduces_object_index_out_of_band`,
 `restore_object_entry_removal_of_unknown_assertion_is_a_noop`) and
 `tests/knowledge_kernel_tests.cpp` (`neighbors_respects_max_hops`,
 `neighbors_returns_empty_for_unknown_subject`, `neighbors_deduplicates_and_avoids_cycles`,
 `neighbors_follows_incoming_edges_reverse_direction`, `neighbors_excludes_non_current_edges`,
 `neighbors_reverse_edges_are_restored_after_kernel_restart` (exercises the fast-path bulk-seed
 specifically), `co_occurring_predicates_returns_currently_active_predicates_for_subject`,
 `co_occurring_predicates_excludes_hypothesis_and_superseded`).
* `KernelCommand`/`KernelResult` now cover all four Phase 7 methods: `CommitHypothesisCommand`
 (mutating, alongside `RecordProvenanceCommand`) and `HypothesesForCommand`/`NeighborsCommand`/
 `CoOccurringPredicatesCommand` (query, alongside `ProvenanceForCommand`). `NeighborsCommand` carries
 `max_hops` explicitly rather than relying on `neighbors`'s default argument — commands are plain,
 fully-specified data, so there is no default-parameter concept for a serialized call. `KernelResult`
 gained one new alternative, `std::vector<EntityId>`, shared by both `neighbors` (`vector<EntityId>`)
 and `co_occurring_predicates` (`vector<PredicateId>`) — the same uint64_t-alias collapsing already
 used for the scalar id alternatives applies equally to vectors of them. This closes out **Phase 7 in
 full**: every method introduced by this phase now has command-layer coverage, matching Phase 6's
 established pattern.
* Covered by additions to `tests/kernel_command_tests.cpp`: `commit_hypothesis_command_round_trips`
 (twin kernels, like the other mutating round-trips) and `hypotheses_for_command_round_trips`/
 `neighbors_command_round_trips`/`co_occurring_predicates_command_round_trips` (same-kernel direct-vs-
 `execute` comparisons, like the other query round-trips).
* Verified on 2026-07-18: `cmake --build build && ctest --test-dir build --output-on-failure` passes
 (16/16 test binaries).

### Phase 8 — Self-Improvement: Merge & Prune

Future work :

* ✅ Entity merge (`merge_entities`) — one-way, append-only redirect for deduplicating entities
* ✅ `Catalog::resolve(EntityId)` — transitive redirect resolution at the query boundary
* ✅ Segment archival (`archive_segments_before`) — compaction, not deletion

**Phase 8 is complete.** All three deliverables above are implemented, tested, documented, and (where
applicable) covered by the `KernelCommand`/`KernelResult` layer.

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

Current implementation status :

* `EntityMergeLog` (`include/kernel/entity_merge_log.hpp`/`src/entity_merge_log.cpp`) persists
 `{EntityId absorbed, EntityId surviving, Timestamp merged_at}` to `catalog/entity_merges.log`.
 Fixed-size record (24 bytes), same framing style as `ObservedTimeIndexLog` rather than the
 variable-length catalog/provenance logs, since every field is a fixed-width integer. Authoritative
 like the other catalog logs: no `overwrite_all`/self-heal, and non-tail corruption is a fatal thrown
 `std::runtime_error`. See `docs/storage_format.md`'s new "Entity merge log" section.
* `StorageConfig` gained `entity_merge_log_path()` (`catalog/entity_merges.log`). `StorageEngine` owns
 the log and exposes `append_entity_merge_entry(absorbed, surviving, merged_at)`/`load_entity_merges()`
 (no `rewrite_*`, consistent with there being no self-heal).
* `Catalog` gained `merge_redirects_` (a plain `absorbed -> surviving` map, one hop only — it does not
 itself collapse chains), `add_merge(EntityId absorbed, EntityId surviving)` (the single mutation entry
 point, used identically for a fresh merge commit and full replay, mirroring `add_entity`/
 `add_predicate`), and `resolve(EntityId) -> EntityId` (follows redirects transitively until reaching
 an id with no outgoing redirect; identity for an unmerged id). `resolve` guards against a malformed
 redirect cycle with a visited-set check and returns the last id reached rather than looping forever —
 defensive only, since merges are meant to be forward-only and a cycle is never expected in practice.
* `KnowledgeKernel::merge_entities(EntityId keep, EntityId absorb, Timestamp merged_at)` is
 durable-before-visible (`storage_.append_entity_merge_entry` then `catalog_.add_merge`), exactly like
 `commit`'s append-then-apply ordering. **Deliberate divergence from the design sketch above:**
 `merge_entities` takes a caller-supplied `merged_at` `Timestamp` rather than reading a wall clock
 internally, matching the same precedent already set by `record_provenance`'s `recorded_at` (no code
 path anywhere reads a clock, keeping replay deterministic). `KnowledgeKernel::resolve_entity(EntityId)
 const` is a thin public wrapper over `catalog_.resolve`.
* Entity-merge replay lives in the same uncaught, authoritative constructor block as the catalog logs,
 payload store, and provenance log — before the snapshot/checkpoint/tail-vs-full-replay branching that
 only concerns the derived Phase 3 indexes.
* **Scoping decision, not in the original design sketch:** resolution is applied only to the
 `KnowledgeKernel` query methods that take a caller-supplied *subject* `EntityId` used to look up
 assertions — `assertions_for_subject`, `current`, `hypotheses_for`, `neighbors`,
 `co_occurring_predicates`, `valid_at`, `known_at`, `valid_at_known_at`, `valid_time_timeline`,
 `observed_time_timeline`, `commit_history`, `find_conflicts` — each resolving its `subject` parameter
 via `catalog_.resolve` before doing anything else. Catalog name/value lookups (`entity_name`,
 `entity_value`, `predicate_name`, `find_entity`, `find_value`, `find_predicate`, `document_content`)
 are deliberately **not** resolved: they answer "what is this id called/worth," a fact about the
 specific id, not "which real-world entity does this id canonically represent." Write-path methods
 (`commit`/`commit_superseding`/`commit_retraction`/`commit_hypothesis`/`intern_*`) are also not
 resolved, consistent with "assertions are never rewritten by a merge" — a caller who wants new
 commits filed under the canonical id is expected to resolve first, the same way commands already
 expect callers to intern before committing.
* `KernelCommand`/`KernelResult` gained `MergeEntitiesCommand` (mutating, alongside
 `RecordProvenanceCommand`/`CommitHypothesisCommand`) and `ResolveEntityCommand` (query, collapsing to
 the existing `AssertionId` alternative like every other scalar-id command).
* Covered by `tests/entity_merge_log_tests.cpp` (append/read round trip, missing file, invalid header,
 partial header, invalid record size, tail checksum mismatch, non-tail checksum mismatch, incomplete
 trailing record — the standard per-log battery, minus `overwrite_all` since there is none), additions
 to `tests/catalog_tests.cpp` (`resolve_is_identity_for_an_unmerged_id`,
 `resolve_follows_a_merge_redirect`, `resolve_collapses_transitive_merge_chains`), additions to
 `tests/knowledge_kernel_tests.cpp`
 (`merge_entities_makes_queries_for_the_absorbed_id_resolve_to_the_surviving_id`,
 `merged_entity_redirect_is_preserved_across_kernel_restarts`, `assertions_are_not_rewritten_by_a_merge`,
 `corrupt_entity_merge_log_is_fatal_on_startup`), and additions to `tests/kernel_command_tests.cpp`
 (`merge_entities_command_round_trips`, `resolve_entity_command_round_trips`).
 `examples/knowledge_kernel_demo.cpp` gains an "Entity merge" section.
* `AssertionLog::archive_segments_before(AssertionId)` moves every already-rolled-from segment
 entirely before the given id (its exact end id, `(index + 1) * max_records_per_segment`, `<`
 the threshold) from `segments/` into a new `segments/archive/` subdirectory via
 `std::filesystem::rename`, skipping the active segment unconditionally regardless of the threshold —
 it may still receive writes, so it is never guaranteed complete the way an already-rolled-from
 segment is. Idempotent: a segment already moved simply doesn't reappear in a later top-level scan, so
 a repeat call is a no-op for it.
* `AssertionLog`'s internal segment-path/index-listing helpers were generalized to treat
 `segments/archive/` as an equally valid home for any given segment index: `segment_path` now checks
 the top-level location first and falls back to the archive location, and `existing_segment_indices`
 (used by `read_all`/`read_after`/`record_count_hint`) now merges indices from both directories. A
 segment lives in exactly one of the two locations at a time, so this needed no de-duplication logic.
 The net effect is that `read_all`/`read_after`/`record_count_hint` are completely unaffected by
 archival — the public `AssertionLog` interface gained only the one new method. `StorageEngine`/
 `KnowledgeKernel` expose the same operation 1:1 as `archive_segments_before`, with no new storage of
 their own (unlike every other Phase 5-8 addition, this needed no new log/persisted state at all).
* Covered by additions to `tests/assertion_log_tests.cpp`
 (`assertion_log_archive_segments_before_moves_only_fully_rolled_segments`,
 `assertion_log_archived_segments_remain_readable_via_read_all`), a
 `tests/knowledge_kernel_tests.cpp` integration test
 (`archive_segments_before_is_transparent_to_queries_and_survives_restart`, using a small
 `StorageConfig::max_records_per_segment` to force multiple segments), and a
 `tests/kernel_command_tests.cpp` round-trip test (`archive_segments_before_command_round_trips`) for
 the new `ArchiveSegmentsBeforeCommand`.
* Verified on 2026-07-19: `cmake --build build && ctest --test-dir build --output-on-failure` passes
 (17/17 test binaries).

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

Current implementation status :

* Benchmark harness added ahead of any actual optimization, per the Performance Rules' "add or update a
 benchmark, record a baseline" first step. `benchmarks/benchmark_harness.hpp` is a small header-only
 `Timer`/`report()` pair shared by three executables (`benchmarks/commit_benchmark.cpp`,
 `query_benchmark.cpp`, `replay_benchmark.cpp`), matching the hand-rolled, no-framework style already
 used for `tests/`. They are wired into `CMakeLists.txt` as plain `add_executable` targets, deliberately
 **not** registered via `add_test`/`ctest`, since they report throughput/latency numbers rather than
 pass/fail — building/running them is a manual step (see `docs/benchmarks.md`).
* `commit_benchmark` measures sequential `commit()` throughput (distinct subjects, isolating the
 append/index/checkpoint path from supersession bookkeeping) and `commit_superseding()` throughput
 (repeated supersession of one subject/predicate, adding `mark_superseded`'s current-index tombstone
 write). `query_benchmark` measures `current`/`valid_at`/`known_at`/`neighbors` throughput over a
 5,000-subject pre-populated kernel. `replay_benchmark` compares startup/reopen cost across three
 scenarios against the same populated log: trusted persisted indexes (normal fast path), a forced full
 replay (checkpoint file deleted, same fallback trigger the `corrupt_*_falls_back_to_replay_and_self_
 heals` tests use), and a reopen immediately after an explicit `write_snapshot()` call.
* The default `build/` tree stays `Debug` (unoptimized) for day-to-day development; benchmarks must be
 built in a separate `Release` tree (`cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release`) to
 produce meaningful numbers — `CMakeLists.txt` was not changed to default to `Release`, so the existing
 test/debug workflow is unaffected.
* Baseline numbers recorded 2026-07-20 (single run each, this container): `commit()` ~67-69 commits/sec
 flat across 1k/10k (fsync-dominated, not O(n)); reads are 4-6 orders of magnitude faster than commits
 (hundreds of thousands to low millions of queries/sec, no durability write on the read path); forced
 full replay is ~13-30x slower than a trusted-index reopen at the 2k-5k record scale tested, though at
 that scale it's dominated by the three index-log rewrites' fixed fsync cost rather than the O(n)
 `apply()` loop. Full numbers, methodology, and a per-Phase-9-candidate read of what each measurement
 does and doesn't cover live in `docs/benchmarks.md` — this container's `fsync` latency is unusually
 high, so absolute numbers should be re-measured on real target hardware before being trusted; relative
 deltas from a future before/after optimization comparison are what the Performance Rules workflow
 actually needs.
* No optimization work (SIMD, mmap, lock-free readers, NUMA allocation, background compaction, Bloom
 filters, compression) has been implemented yet — this status block covers only the benchmark/baseline
 groundwork the Performance Rules require before any of that can start.

-- -

## Architectural Principles

### 1. The log is the source of truth

The assertion log records committed knowledge changes.

In - memory vectors, indexes, and caches are derived state.

Do not make indexes authoritative.

There is a second, recognized category distinct from both: **authoritative metadata logs**
(`Catalog`'s `EntityCatalogLog`/`PredicateCatalogLog`, Phase 6's `ProvenanceLog`, Phase 8's
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

## Batch Commits

**Added 2026-09-06, for #51.** `KnowledgeKernel::commit_batch` appends many new Active assertions in
one call. Everything about the records it writes is identical to committing them one at a time — the
same frames in `assertions.log`, the same three index-log entries each, ids assigned consecutively in
input order. The only thing that changes is how many durability boundaries the work costs: one fsync
per underlying log for the whole batch (5 total: assertion log, three index logs, checkpoint) rather
than 5 per assertion.

**What is the invariant?** Durability is *prefix-shaped*, not all-or-none. A crash mid-batch leaves
the first k entries durable for some 0 <= k <= N, never a gap and never a reordering. Everything else
a single commit guarantees still holds unchanged: durable-before-visible ordering (assertion log,
index logs, checkpoint, then `apply()`), no burned ids on a rejected call, and append-only records.

**Why this design?** True all-or-none would need a batch-commit marker record in the log, i.e. a new
record type and a format version bump for every reader, plus replay logic that discards trailing
records not covered by a marker. Rather than claim an atomicity the format does not support, this
promises exactly what it does: because records are written in order and only the active segment's
trailing frame can be torn, the surviving prefix is a contiguous id range, so a caller can determine
k exactly and resume at input index k instead of guessing. #51 explicitly allows this trade
("or — if all-or-none is the wrong bargain for an append-only log — reports precisely how many were
applied and in what order"). Revisit only alongside a deliberate format-version change.

**What are the failure modes?**

* Crash mid-batch — a prefix is durable; the checkpoint is written last, so it lags the log and the
  next startup rebuilds every index from the log, the same self-heal a torn single commit gets.
* Over-sized batch — rejected with `std::runtime_error` before anything is built or appended, so no
  id is burned and no record is written. `MAX_BATCH_SIZE` is a bound on caller-supplied input, not a
  tuning knob: an unbounded list must never become an unbounded write.
* Empty batch — a no-op returning an empty vector; no log file or segment is created.

**How is it tested?** `tests/knowledge_kernel_tests.cpp` (id order and continuation, per-entry valid
time, index parity with single commits, restart, over-sized rejection, empty batch, a batch spanning
segment boundaries read back after a forced full replay; and for the name-based overload: interning
and input order, reuse of existing catalog ids across repeated batches, restart, and an over-sized
batch interning nothing), `tests/assertion_log_tests.cpp` and the
three index-log test files (`append_batch` record order, empty-batch no-op, and the segments-stay-
exactly-full invariant mid-batch), `tests/kernel_command_tests.cpp`, and `tests/mcp_tools_tests.cpp`.
`benchmarks/commit_benchmark.cpp` measures it against the identical `commit()` workload.

**The name-based overload.** `commit_batch_by_name` (added 2026-09-06 alongside the above) stands to
`commit_batch` exactly as `commit_by_name` stands to `commit`: it interns each entry's subject name,
predicate name, and object `Value`, then calls `commit_batch` with the resolved entries. Pure
composition of existing idempotent primitives — no new storage, no new invariants, and every
guarantee above carries over unchanged.

One thing does not carry over, and is worth stating plainly: **interning happens before the batch's
durability boundary**, so each genuinely new name is its own catalog append and fsync. A batch of all-
new names therefore costs one fsync per new name plus the batch's own five; the case this exists for
— restating a field for subjects the kernel already knows, under one predicate — interns nothing new
for the subjects and at most one new predicate, which is why it is affordable. A crash partway
through interning is harmless rather than partial: catalog entries are id/name mappings with no
assertion attached yet, and interning is idempotent, so a retry reuses the same ids and commits
again. The `MAX_BATCH_SIZE` check runs before any interning, so a rejected batch interns nothing.

**Deliberately out of scope.** A batch holds plain appends only: no supersession, retraction, or
hypothesis entries, and no per-entry provenance. Each of those carries target validation or a second
durable write that would have to interleave with the batch's single boundary — a different and much
less obviously correct feature. Returning ids in input order is what lets a caller record provenance
itself afterwards without a lookup per assertion. Note also that this is emphatically **not** a
predicate merge: #51 asks for restating a field as new appends precisely because merging predicates
would retroactively change the meaning of assertions people already made under a name they chose,
which is the one thing the append-only guarantee exists to prevent. There is no predicate merge and
this section does not introduce one.

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

std::vector<Assertion> changes_since(
    Timestamp observed_since
) const;
```

`changes_since` is the one query method above that is not scoped to a `subject`: it is the
kernel-wide, status-agnostic answer to "what changed since t" (any subject, any predicate, sorted
by `observed_at`). It belongs with `commit_history`/`observed_time_timeline` in the "full audit
history" group below, not with `current`/`valid_at`/`known_at`.

`current(subject)` only ever answers in the subject -> object direction. Two companions close the
other directions an application (e.g. a CRM asking "who works at Acme," or a retriever pulling
every `WORKS_AT` edge to build a subgraph for an agent) needs and previously had no query for at
all:

* `current_by_object(EntityId object) -> vector<Assertion>` — the reverse-direction counterpart to
 `current(subject)`. Resolves `object` through `Catalog` first, exactly like `current` resolves
 `subject`, so merge transparency applies symmetrically.
* `current_by_predicate(PredicateId predicate) -> vector<Assertion>` — kernel-wide, any subject, any
 object; e.g. every currently-active `WORKS_AT` assertion.

Both are backed by `IndexManager` structures that already existed or fit naturally alongside
existing ones: `current_by_object` reuses the reverse object index Phase 7's `neighbors` built
(previously internal-only); `current_by_predicate` is backed by a new `predicate_current_index_`
folded directly into `restore_current_index_entry`, so — unlike the object index — it needs no
separate bulk-seed step on the fast startup path: predicate is already part of every persisted
`current_index.log` record.

Interval semantics:

```text
valid_from <= t < valid_to
```

For open - ended intervals:

```text
valid_to == OPEN_ENDED
```

means the assertion remains valid until closed, superseded, or retracted.

**A subtlety worth being explicit about, surfaced while writing `examples/agent_workflow_demo.cpp`:**
`known_at`/`valid_at_known_at` filter on `status == Active` at query time, not status as of
`observed_time`. They answer "of what had been observed by `t`, what do we *currently* still
consider Active" — not "what would `current()`/`valid_at()` have returned if called back at `t`."
Concretely: if assertion A was observed at `t0` and later superseded at `t1 > t0`, then
`known_at(subject, t)` for any `t` with `t0 <= t < t1` returns A right up until `t1`, but returns
*nothing* for A once queried after `t1` — even for the same historical `t` cutoff, since A is no
longer Active. There is no persisted "status as of a past commit" to query instead; reconstructing
that would require replaying the log up to a given commit point, which is not something either
method does today. Not a bug — the existing tests (`known_at_excludes_future_observed_fact`,
`valid_at_known_at_respects_both_times`) already lock in the current-status-filtered behavior — but
worth knowing before reaching for these methods to answer "what did we believe back then," which
they do not actually answer once a later correction has landed.

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

**Added 2026-08-15: the single-writer half of this model is now enforced, not just declared.**
`StorageEngine`'s constructor takes an advisory exclusive lock (POSIX `flock()`) on the storage
root for its lifetime (`kernel/storage_lock.hpp`), so a second concurrent open of the same root —
in `mcp_server`, or any caller linking `libkernel.a` directly — fails fast with a clear error
instead of silently corrupting the log. This is enforcement of the already-declared model, not new
concurrency: readers still don't exist, so every open (read or write) takes the exclusive lock
today; a read-only open can switch to a shared lock additively once "many readers later" lands. See
`docs/storage_format.md`'s "Storage root lock" section.

Do not add concurrency prematurely.

When concurrency is introduced:

* Reads must see consistent snapshots.
* Commits must be durable before visible.
* Index updates must be atomic relative to query visibility.
* Replaying the log must be deterministic.

-- -

## MCP Parameter Ordering

**Added 2026-08-15, after #47.** `mcp_tools.cpp`'s `object_schema` property list is a *signature*,
not a set. Declare parameters in signature order with **all optional parameters last**, and list
`required` as a **prefix** of that order — e.g. `changes_since`'s schema declares
`observed_since, limit, newest_first` with `required = [observed_since]`, not `limit, newest_first,
observed_since` even though that would be equally valid JSON Schema.

This is a wire-format guarantee, not a style preference: a treelang-style caller binds tool
arguments positionally against the *emitted* `properties` order and takes a prefix for the values
it supplies, so an optional parameter declared ahead of a required one silently shifts every
argument after it — see #47 for the incident (introduced by #43's first optional MCP parameters,
where `nlohmann::json`'s default `std::map`-backed object re-sorted `properties` alphabetically on
serialization, moving `limit`/`newest_first` ahead of the required parameters they were declared
after). `ToolSpec::input_schema` and every property-builder helper in `mcp_tools.cpp` use
`nlohmann::ordered_json` for exactly this reason — do not change them back to `nlohmann::json`, and
if a new JSON container sits between a schema and the wire (as `mcp/main.cpp`'s `tools/list`
envelope does), it has to stay `ordered_json` all the way to `dump()` too, since assigning an
`ordered_json` into a plain `json` object re-sorts it right back.

`tests/mcp_tools_tests.cpp`'s `every_tool_schema_emits_required_as_a_prefix_of_properties_in_declared_order`
guards this against the *serialized* schema (dump then re-parse with `ordered_json`), not the
declared one, since the declarations were never wrong — only the serialization path was.

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

Two of these items are narrowed, not lifted, by Phase 7 (see "Current Roadmap" above) :

* **complex graph traversal** stays forbidden as a general capability. Phase 7's `neighbors`/
 `co_occurring_predicates` are the one named, explicitly bounded exception (small fixed hop count, no
 path queries, no joins) — a general graph query language is still out of scope.
* **LLM extraction** has always meant *the kernel performing NLP extraction from raw text*. It does
 not mean *an external agent or LLM writing already-structured assertions through the public API* —
 that has always been in scope; Phase 6's `KernelCommand` layer and Phase 7's `commit_hypothesis` are
 both just structured calls into the existing commit machinery, not extraction happening inside the
 kernel. Written down explicitly here because Phase 7 is where the ambiguity would otherwise bite.

**HTTP API / network transport** stays fully out of scope: no listening socket, no request routing,
no auth/TLS surface has been added. This item is about a *network* boundary specifically, not about
whether the kernel has any caller-facing boundary at all.

A **local MCP server over stdio** (`mcp/`, see below) is a narrower, deliberate exception, not a
lifting of this item: it is a subprocess speaking newline-delimited JSON-RPC on its own stdin/stdout,
with no socket, no network exposure, and no auth model beyond "whatever process spawned it controls
its stdio." It exists because Phase 6's `KernelCommand`/`KernelResult` layer was built specifically to
make a boundary like this possible, and "no application can call the kernel from outside a C++
process linking `libkernel.a`" was identified as the concrete blocker to building anything (a CRM, an
agentic retriever) on top of the kernel at all. A real network-facing HTTP/gRPC API is still a
separate, not-yet-made decision.

Implementation:

* `third_party/nlohmann/json.hpp` — this project's first external dependency: a single vendored
 header (nlohmann/json v3.12.0, MIT license, fetched from the upstream `single_include` release, not
 a moving branch). `KernelCommand`/`KernelResult` together span 40 command shapes and 11 result
 shapes with varied field types (ids, timestamps, raw bytes, the tagged `Value` union); a hand-rolled
 JSON parser correctly covering escaping/unicode/number formats for all of that was judged real,
 bug-prone surface for no benefit over a well-tested single-header library. `target_include_directories(kernel PUBLIC third_party)` in `CMakeLists.txt` makes `#include
 <nlohmann/json.hpp>` resolve; nothing else in `third_party/` is expected to appear without the same
 kind of explicit justification.
* `include/kernel/json_codec.hpp` / `src/json_codec.cpp` — transport-agnostic JSON (de)serialization
 for `Assertion`, `Value`, `ProvenanceRecord`, `AssertionStatus`, and a `kernel_result_to_json`
 covering all 11 `KernelResult` alternatives via `std::visit`. Deliberately does **not** include a
 generic tagged-variant codec for `KernelCommand` itself — the one caller (`mcp_tools`) already knows
 which command it's building from the MCP tool name, so each tool constructs its specific `Command`
 struct directly. Also owns hand-rolled base64 encode/decode for the one raw-bytes field in the
 surface (`InternDocumentCommand::content` / `DocumentContentCommand`'s return) — JSON has no native
 binary type.
* `include/kernel/mcp_tools.hpp` / `src/mcp_tools.cpp` — the pure, I/O-free half of the server: one
 MCP tool per `KernelCommand` variant (42 total, after `commit_by_name` and `current_by_name` — see
 Phase 5's "Current implementation status" — added the 39th and 40th, and `commit_batch`/
 `commit_batch_by_name` the 41st and 42nd),
 named after the mirrored
 `KnowledgeKernel` method
 (`"commit"`, `"current_by_object"`, ...), each with a JSON Schema `inputSchema` built from its
 fields. Deliberately not a single generic "execute a `KernelCommand` blob" tool — MCP tool schemas
 are meant to be individually discoverable and typed by an agent, which a polymorphic tool would
 defeat. `handle_tool_call` looks up the tool, builds the command from the arguments, calls
 `kernel.execute(...)`, and serializes the result; an unknown tool name, a missing/malformed
 argument, or an exception from `execute()` itself (e.g. `commit_retraction` against an unknown id)
 all come back as `ToolCallResult{.is_error = true}` rather than throwing, matching MCP's convention
 of reporting a tool-level failure as a normal JSON-RPC success with `isError: true`, not a
 JSON-RPC-level error. Directly unit tested (`tests/mcp_tools_tests.cpp`), including a completeness
 check that every `KernelCommand` variant has exactly one corresponding tool.
* `mcp/main.cpp` — the thin stdio JSON-RPC 2.0 loop: one message per line on stdin/stdout (MCP's
 stdio framing; no `Content-Length` headers, unlike LSP). stdout is reserved entirely for protocol
 messages; anything diagnostic goes to stderr. Handles `initialize`, `notifications/initialized`
 (and `notifications/cancelled`, both no-response), `tools/list`, `tools/call`, and reports unknown
 methods/malformed requests as JSON-RPC errors (`-32601`/`-32600`/`-32602`/`-32700`). Takes the
 storage root as its one required CLI argument: `./build/mcp_server <storage-root>`. Not unit
 tested, mirroring how other executable entry points in this repo (`kernel_demo`, the benchmarks)
 aren't — verified instead by piping JSON-RPC requests into the built binary manually and checking
 the responses.

The full tool list, an example JSON-RPC session, and how to point a real MCP client at the built
binary live in `docs/mcp_server.md` — this section covers the design decisions, that one covers usage.

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
for Phase 7's `hypotheses_for`/`commit_history` combination.

"What changed since yesterday?" also remained only partially answered even after Phase 6/7:
`commit_history`/`observed_time_timeline` require a caller-supplied `subject`+`predicate`, so a
caller had no way to discover *what* changed without already knowing where to look — the
`examples/agent_workflow_demo.cpp` scenario papered over this by manually filtering
`commit_history(alice, works_at)` client-side, which only worked because the caller already knew
to ask about Alice. `changes_since(Timestamp)` closes this properly: a kernel-wide (not
per-subject), status-agnostic scan mirroring `commit_history`'s full-audit-history philosophy (new
commits, supersessions, retractions, and hypotheses all count as a "change"). Implemented as a
straight scan over `assertions_`, not a new index — there is no persisted global observed-time
ordering (`IndexManager`'s observed-time index is per-subject), and per the Performance Rules below
that's not worth adding without a demonstrated bottleneck. Covered by
`changes_since_returns_assertions_observed_at_or_after_cutoff_sorted_by_observed_at` and
`changes_since_is_status_agnostic_and_spans_multiple_subjects` in
`tests/knowledge_kernel_tests.cpp`, plus `changes_since_command_round_trips` in
`tests/kernel_command_tests.cpp` (`ChangesSinceCommand`, dispatched in `execute()` like every other
query command). `examples/agent_workflow_demo.cpp` now calls `changes_since` directly instead of
the client-side filter.

Every implementation decision should support this long - term direction.

The kernel is not merely storing facts.

It is preserving the history of how knowledge changes.
