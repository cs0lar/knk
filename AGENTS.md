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

### Phase 2 — Storage engine/current phase

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

### Phase 3 — Persistent indexes

Future work :

	* Subject index
	* Predicate index
	* Current - state index
	* Observed - time index

	Indexes are derived acceleration structures. They must be rebuildable from the assertion log.

	If persistent indexes are missing or corrupted, the kernel should still recover from the log.

### Phase 4 — Storage engine internals

Future work :

	* Segment files
	* WAL
	* Checksums
	* Crash recovery
	* Snapshots

	At this phase, the log should evolve from a simple file into a segmented storage subsystem with integrity checks and controlled recovery.

### Phase 5 — Performance

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

Do not introduce templates, custom allocators, memory mapping, SIMD, or lock - free structures before Phase 5.

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

Phase 5 performance work must be benchmark - driven.

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
  ```

  Every implementation decision should support this long - term direction.

  The kernel is not merely storing facts.

  It is preserving the history of how knowledge changes.
