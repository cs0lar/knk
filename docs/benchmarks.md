# Benchmarks

This document describes the `benchmarks/` executables and records baseline numbers, per AGENTS.md's
Performance Rules: *"Before adding an optimization: 1. Add or update a benchmark. 2. Record baseline
behavior. 3. Implement optimization. 4. Compare results. 5. Keep correctness tests passing."* These
three benchmarks are that step 1/2 groundwork for Phase 9 — no optimization work has happened yet.

## Running

Benchmarks are plain executables (same "no framework, hand-rolled" style as `tests/`), not wired into
`ctest`, since they report numbers rather than pass/fail. Build them in a `Release` tree — the default
`build/` directory used for day-to-day development is `Debug` (unoptimized, `-g`), which is not
representative of real performance:

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --target commit_benchmark query_benchmark replay_benchmark
./build-release/commit_benchmark
./build-release/query_benchmark
./build-release/replay_benchmark
```

## What each one measures

* **`commit_benchmark`** — sequential `commit()` throughput with distinct subjects/objects per call
  (isolates the append/index/checkpoint path from supersession bookkeeping), and
  `commit_superseding()` throughput against a single, repeatedly-superseded subject/predicate (adds
  `mark_superseded`'s current-index removal on every call).
* **`query_benchmark`** — read-path throughput (`current`, `valid_at`, `known_at`, `neighbors`) over a
  kernel pre-populated with 5,000 subjects, each with a `WORKS_AT` and a `LIVES_IN` assertion (the
  latter chained subject-to-subject so `neighbors` has real edges to walk).
* **`replay_benchmark`** — startup/reopen cost under three scenarios against the same populated log:
  trusted persisted indexes (the normal fast path), a forced full replay (checkpoint deleted, so
  every record goes through `apply()` and all three indexes get rebuilt and rewritten — the same
  fallback trigger the `corrupt_*_falls_back_to_replay_and_self_heals` tests exercise), and a reopen
  immediately after an explicit `write_snapshot()` call.

## Baseline results (2026-07-20)

Measured in this container on a single run each (no statistical repetition/warm-up beyond what's
already built into each benchmark's own loop); treat as a rough baseline to compare future runs
against, not a tuned number. Notably, `fsync` latency on this container's filesystem is unusually high
(~15 ms/call) — the throughput and replay numbers below are dominated by that, not by kernel logic.
Re-run before trusting absolute numbers on different hardware; relative deltas between before/after an
optimization are what matter per the Performance Rules workflow.

### `commit_benchmark`

```
== commit() -- distinct subjects ==
commit_throughput(1000)                                 66.527 commits/sec
commit_throughput(1000) avg latency                     15.031 ms/commit
commit_throughput(10000)                                68.675 commits/sec
commit_throughput(10000) avg latency                    14.561 ms/commit

== commit_superseding() -- same subject/predicate ==
commit_superseding_throughput(1000)                     57.323 commits/sec
commit_superseding_throughput(10000)                    57.413 commits/sec
```

`commit()` is flat at ~67-69 commits/sec regardless of scale (1k vs 10k) — consistent with per-commit
`fsync` being the dominant cost, not any O(n) in-memory work. `commit_superseding()` is ~15% slower
per call, the added cost of one extra current-index tombstone write plus `mark_superseded`.

### `query_benchmark`

```
== query throughput over 5000 subjects ==
current(subject)                                   3857588.331 queries/sec
valid_at(subject, t)                              12380356.237 queries/sec
known_at(subject, t)                               8698033.166 queries/sec
neighbors(subject, max_hops=2)                      544793.689 queries/sec
```

Reads are 4-6 orders of magnitude faster than commits, as expected: no durability write on the query
path, just in-memory hash-map/index lookups. `neighbors` is the slowest of the four by a wide margin
(still ~545k/sec) since it does a bidirectional BFS instead of a single index lookup — the one query
method worth watching if graph-traversal usage grows in a future phase.

### `replay_benchmark`

```
== startup/replay cost ==
reopen, trusted indexes (2000 records)                   2.373 ms
reopen, forced full replay (2000 records)               30.520 ms
reopen, after write_snapshot (2000 records)              2.333 ms
reopen, trusted indexes (5000 records)                   5.468 ms
reopen, forced full replay (5000 records)               31.621 ms
reopen, after write_snapshot (5000 records)              4.033 ms
```

The trusted-index fast path scales roughly linearly with record count (2.4 ms at 2k, 5.5 ms at 5k), as
expected for an O(n) `restore_assertion` pass plus one in-memory object-index rebuild. Forced full
replay is ~13-30x slower and roughly *flat* across 2k-5k records — at this scale it's dominated by the
fixed `fsync` cost of the three index-log `rewrite_*` calls and the checkpoint write it performs
afterward, not by the O(n) `apply()` loop itself; the O(n) term should start to dominate at larger
record counts than tested here. The post-`write_snapshot` reopen is the fastest of the three at 5k
records (4.0 ms vs. 5.5 ms trusted-only) since it bulk-loads `assertions_` from one snapshot file
instead of parsing 5,000 individual log records, though there's little tail to skip in this scenario
(no commits happened between the snapshot and the reopen) — a log with a large post-snapshot tail
would show a smaller relative gain.

## Reading these numbers against Phase 9's candidate list

* **SIMD scanning / Bloom filters** — no linear scan exists on the hot query path today (`current`/
  `valid_at`/`known_at` all resolve through `IndexManager`'s hash maps), so neither has an obvious
  target yet; `query_benchmark`'s per-method numbers are the baseline to check any future scan-shaped
  addition against.
* **Memory-mapped segments** — would target `commit_benchmark`'s and `replay_benchmark`'s I/O-bound
  numbers (fsync latency and full-log-parse cost), not the query path.
* **Lock-free readers / NUMA allocator** — not measurable yet; the kernel is single-writer/no-declared-
  concurrency today (see AGENTS.md's Concurrency Rules), so there's no concurrent-access benchmark to
  baseline until that model exists.
* **Background compaction** — `archive_segments_before` has no dedicated benchmark yet; would be a
  natural fourth addition once compaction-triggered work is actually being considered.
* **Compression** — a size, not a latency/throughput, concern; would need a separate on-disk-size
  measurement, not a `Timer`-based benchmark like the three here.
