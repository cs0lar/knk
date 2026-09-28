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
  (isolates the append/index/checkpoint path from supersession bookkeeping),
  `commit_superseding()` throughput against a single, repeatedly-superseded subject/predicate (adds
  `mark_superseded`'s current-index removal on every call), and `commit_batch()` throughput over the
  *same* workload as the first of those — identical records, identical index updates, issued as one
  call instead of N, so the difference is purely the collapsed durability boundary. A fourth section
  attaches provenance to a committed batch both ways (`record_provenance` per assertion vs. one
  `record_provenance_batch`) against the same batch, isolating the provenance log's fsync count.
* **`query_benchmark`** — read-path throughput (`current`, `valid_at`, `known_at`, `neighbors`) plus the
  query IR's own paths (index-selected, forced-scan, filtered, and name-resolving) over a
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

The `commit_batch()` section of this benchmark did not exist on 2026-07-20; its numbers were recorded
separately (below) and are **not** comparable to the figures above, which came from a much
slower-`fsync` machine.

### `commit_benchmark` — `commit_batch()` vs `commit()` (2026-09-06)

Recorded when `commit_batch` was added, on different hardware from the 2026-07-20 baseline above
(this machine's `fsync` is ~300x faster, which is why `commit()` reads ~20,000/sec here rather than
~67/sec). Only the *ratio* within this run is meaningful; it does not supersede the baseline above.

```
== commit() -- distinct subjects ==
commit_throughput(1000)                              19829.113 commits/sec
commit_throughput(10000)                             20070.836 commits/sec

== commit_batch() -- distinct subjects, one durability boundary ==
commit_batch_throughput(1000)                      1491916.053 commits/sec
commit_batch_throughput(1000) whole batch                0.670 ms/batch
commit_batch_throughput(10000)                     1344881.743 commits/sec
commit_batch_throughput(10000) whole batch               7.436 ms/batch
```

Same records, same index writes, ~67x the throughput: 10,000 assertions take 7.4 ms as one batch
versus ~498 ms as 10,000 commits. The gap is entirely the fsync count — a batch performs 5 fsyncs
total (assertion log, three index logs, checkpoint) regardless of size, where N commits perform 5N.
It follows that the ratio *grows* with `fsync` latency: on the 2026-07-20 container (~15 ms/fsync)
the same 10,000-assertion batch would still be ~5 fsyncs, so the speedup there would be far larger
than the 67x measured here. Per-assertion throughput is slightly lower at 10k than at 1k
(1.34M vs 1.49M/sec), consistent with the batch's O(n) in-memory work (building the record vectors
and `apply()`-ing each entry) becoming visible once the fixed fsync cost is amortized away.

### `commit_benchmark` — provenance for a batch (2026-09-06)

Recorded when `record_provenance_batch` was added, on the same machine and in the same run as the
`commit_batch` numbers above (so directly comparable to them, and to nothing in the 2026-07-20
baseline).

```
== provenance for a committed batch -- per record vs. batched ==
record_provenance x1000                                  8.245 ms
record_provenance_batch(1000)                            0.159 ms
record_provenance x10000                                82.172 ms
record_provenance_batch(10000)                           1.562 ms
```

Attaching provenance one record at a time cost **8.5x the batch it describes** (82 ms against
`commit_batch`'s ~7-10 ms for the same 10,000 assertions) — i.e. provenance was ~89% of the total
work, and was the one place per-assertion fsync cost survived on the batch path.
`record_provenance_batch` collapses it to a single fsync: **~53x faster**, and provenance drops to
~18% of the pair. The ratio grows with `fsync` latency for the same reason `commit_batch`'s does.

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

### `query_benchmark` — the query IR (2026-09-28)

Recorded when Phase 11 added filters, projection and index selection, on the same machine as the
`commit_batch` numbers above (and so not comparable to the 2026-07-20 baseline). Same 5,000-subject,
10,000-assertion corpus as the section above it.

```
== query IR over the same 5000 subjects ==
query{subject,Active,open_ended}                  15124273.127 queries/sec
query{subject,observed_to}                        15087598.597 queries/sec
query{...} resolve_names                          10962205.385 queries/sec
query{...} force_scan                                    0.005 ms/query
query{filter: confidence < 0.5} (matches none)           0.073 ms/query
query{filter: confidence >= 0.5} (matches all)           0.851 ms/query
```

Four things worth reading off these, all of them baselines for later phases to beat:

* **The IR is about 2x faster than the method it reproduces** — 15.1M/sec against `current()`'s
  7.2M/sec for the identical question. Not an optimization, just a shorter path: `current()` walks the
  subject's predicate set and does a current-index lookup per predicate, while the IR takes one
  subject-index lookup and filters. Worth knowing before anyone "optimizes" the IR by routing it back
  through the methods.
* **Index selection is worth ~30x here** (0.005 ms/query scanned versus ~0.000066 ms indexed), and the
  gap grows linearly with corpus size, since the scan is O(total assertions) while the indexed path is
  O(rows for that subject). The differential tests assert the two return the *same* rows; this is the
  cost side of that comparison.
* **Name resolution costs ~27%** on this shape (11.0M/sec against 15.1M/sec) for two catalog lookups
  per returned row. That is the price of not making a second round trip through the batch resolvers,
  and it is charged only on the returned page.
* **Materializing and sorting dominates a broad filter**: the same scan costs 0.073 ms when nothing
  matches and 0.851 ms when everything does — ~12x, entirely from building and sorting 10,000
  `Assertion` copies. Per-row filter evaluation itself is ~7 ns, most of it constructing a `Value` to
  compare against. Both are Phase 15 targets (columnar evaluation avoids the `Value`; a top-k heap
  avoids sorting what the limit will discard).

### `query_benchmark` — aggregation (2026-09-28)

Recorded with Phase 12, same corpus and machine as the section above.

```
== aggregation over the same corpus ==
aggregate{count, no grouping}                            0.077 ms/query
aggregate{count+avg, by predicate}                       0.192 ms/query
aggregate{count, by subject (5000 groups)}               0.970 ms/query
```

The number that matters is the comparison with the row query directly above: counting every row costs
**0.077 ms against the 0.856 ms** a query matching those same 10,000 rows takes — about **11x cheaper**,
because an aggregate folds each row into its group and drops it, while the row query builds and sorts
10,000 `Assertion` copies. That is the streaming claim, measured rather than asserted.

Cost scales with *groups*, not rows: one group is 0.077 ms, two groups with a second aggregation is
0.192 ms, and 5,000 groups from the same 10,000 rows is 0.970 ms. At that width the work is dominated by
constructing a key `Value` per row and inserting into the ordered group map — the natural Phase 15
target, where a columnar pass can compute keys without materializing a `Value`.

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
