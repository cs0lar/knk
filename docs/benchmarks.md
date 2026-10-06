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
  query IR's own paths (index-selected, forced-scan, filtered, and name-resolving), aggregation, and the
  columnar substrate's scan and verification costs, over a
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

### `commit_benchmark` — the cost of maintaining columns (2026-09-29)

Phase 14 adds ten arrays to every commit, so the commit path had to be re-measured rather than assumed
unaffected. Same machine as the 2026-09-06 batch numbers above, so directly comparable to them.

```
                              before Phase 14        after
commit()                      20,071/sec             8,944/sec     (0.050 -> 0.112 ms)
commit_superseding()          17,382/sec             8,448/sec
commit_batch(10,000)          7.44 ms/batch          10.11 ms/batch
```

**Single commits cost ~2.2x more; batches ~1.3x.** That is the honest headline, and two rounds of tuning
got it there from an initial **2.9x**:

* **Columns are not fsynced.** They are derived, so losing unflushed bytes to a crash costs a rebuild and
  never data — and the recovery path already handles both a short column and one lagging the log. Ten
  fsyncs per commit bought nothing. (2.9x -> 2.5x.)
* **The manifest is written in place rather than atomically.** `write_file_atomically` costs two further
  fsyncs (temp file, then parent directory) to protect state that is equally reconstructible; a torn
  manifest is caught by its own checksum, which means "rebuild". (2.5x -> 2.2x.)

**The remaining 2.2x is almost entirely an artifact of this machine's unusually fast `fsync` (~10 µs),
and would be near-invisible where fsync is slow.** What is left is eleven file opens and small writes per
commit, about 0.06 ms here. On the 2026-07-20 container, where a single commit took ~15 ms because fsync
cost ~15 ms, the same 0.06 ms of syscalls would be a **0.4%** regression rather than 120%. Relative costs
on this page only compare within a run, and this is the clearest example of why.

The next lever, if it ever matters, is holding the ten file descriptors open across appends instead of
reopening per commit — deliberately not done here, because it trades ten open file descriptors per store
and a stale-handle case after every rebuild for ~15 µs on a path whose recommended bulk form
(`commit_batch`, still ~1M commits/sec) barely notices.

### `query_benchmark` — the cost model and a corpus too large to cache (2026-09-29)

Phase 16. The planner compares candidate sources on modelled cost, so the constants in that model are
measured rather than guessed. Two index lookups differing only in candidate count separate the fixed
lookup from the per-candidate cost; the two scans give the rest:

```
== cost model inputs ==
index path, ~2 candidates                                0.842 us/query
index path, ~5000 candidates                            26.583 us/query
  => per candidate row                                   5.150 ns
  => per row, columnar scan                              0.910 ns
  => per row, row scan                                   1.540 ns
```

**The ratio is what decides anything: an index row costs ~5.7x a scanned column row**, because it is a
random access into an 88-byte record while the scan streams one contiguous column. So an index wins only
while it yields fewer than roughly one row in six — which is why "use an index whenever one applies" (the
Phase 11 heuristic) is wrong for a common predicate.

Every number above this section was measured on 10,000 assertions, which is cache-resident. Phase 15
flagged that as a reason its 1.7x figure understated the layout, and as a risk for calibrating a planner
against. So Phase 16 added a corpus that does not fit — 250,000 assertions, ~22 MB of records against
~2 MB of one column (three runs, showing the spread):

```
== a corpus too large to cache (250000 assertions) ==
large: scan, columnar            0.376 - 0.458 ms/query      (1.5 - 1.8 ns/row)
large: scan, rows                0.922 - 1.042 ms/query      (3.7 - 4.2 ns/row)
large: the declined index path   1.196 - 1.469 ms/query
```

Three things follow, and the third is the one that matters:

* **The columnar advantage grows with size, as predicted**: ~2.5x here against 1.7x on the small corpus.
  Both per-row costs rise once memory traffic stops being free (0.91 -> ~1.6 ns columnar, 1.54 -> ~4.0 ns
  rows), and the row layout rises faster because it drags 88 bytes through cache to read one field.
* **The constants are calibrated at 10,000 rows and are not portable to this size.** The model would
  now understate both scans, and understate the index path by more, since a random access at 250,000
  rows misses cache where at 10,000 it did not. Directionally the decisions stay right; the absolute
  numbers in a plan are comparable only against each other, which is what `explain_query` documents.
* **The planner's decision was validated rather than asserted.** At this size it chooses the columnar
  scan for a predicate covering a quarter of the corpus, and the index it declined — timed directly via
  `current_by_predicate`, which always uses that index — costs **~1.3 ms against the chosen scan's
  ~0.4 ms**. It was right by roughly 3x.

### `query_benchmark` — vectorized execution (2026-09-29)

Phase 15. Before/after measured **in the same session on the same machine**, which matters: an earlier
draft of this comparison used a number recorded on another day and read a 3x machine-state difference as
a code regression.

```
                                        Phase 14      Phase 15    speedup
scan, current-shaped query              0.015         0.009       1.7x
filter, matches none (10,000 rows)      0.054         0.030       1.8x
filter, matches all (10,000 rows)       0.849         0.131       6.5x
```

Three separate changes, each aimed at a cost an earlier phase had measured and named:

* **Late materialization** is where the 6.5x comes from, and it has nothing to do with SIMD. Filtering,
  sorting and paging now work on 4-byte row indices, and only the rows that survive paging are copied.
  Phase 12 measured a broad query as ~12x dominated by building and sorting 10,000 `Assertion` copies;
  sorting indices and materializing one page removes almost all of it. `partial_sort` finishes the job:
  when a limit discards a tail, ordering that tail is work thrown away.
* **Vectorized column passes** give the 1.7x on scans. Each columnar predicate is one branch-free pass
  over one contiguous column writing a byte mask, instead of re-testing every predicate per 88-byte
  record. Part of that win is not the layout at all: the row path re-tests *absent* predicates on every
  row, while the columnar path simply never runs a pass for a predicate the query did not ask for.
* **No `Value` per row.** Filter comparisons now read the field directly instead of materializing a
  `Value` (a struct containing a `std::string`) per row per comparison, and `ObjectValue` borrows the
  catalog's value rather than copying it. Worth ~1.3x on its own on the filter-only path.

```
== aggregation over the same corpus ==
aggregate{count, no grouping}                            0.066 ms/query   (was 0.077)
aggregate{count+avg, by predicate}                       0.182 ms/query   (was 0.192)
aggregate{count, by subject (5000 groups)}               1.035 ms/query   (was 0.970)
```

Aggregation gains little and the widest case is marginally *worse*: its cost is key construction and
ordered-map insertion, which vectorized selection does not touch, and the selection vector is pure
overhead when nothing narrows it. That is the honest read, and it says where aggregation work belongs
next — hashing group keys without building a `Value` per row — rather than here.

**What did not improve, and why.** At 10,000 rows everything is cache-resident, so these numbers measure
instruction count far more than memory bandwidth; the layout advantage grows on corpora too large to
cache, which this corpus cannot show. No explicit SIMD intrinsics were added: the passes are written
branch-free for the compiler to vectorize, and nothing here yet justifies hand-written intrinsics over
that.

### `query_benchmark` — the columnar substrate (2026-09-29)

Recorded with Phase 14, same 10,000-assertion corpus and machine as the sections above. Nothing queries
columns yet, so these exist to be beaten by Phase 15 rather than to show a win now.

```
== columnar substrate over the same corpus ==
columns: count(confidence >= 0.5)                        0.002 ms/scan
rows:    count(confidence >= 0.5)                        0.005 ms/scan
columns: verify (manifest checksums)                     1.404 ms/open
```

**2.5x for the same predicate, and that understates it at scale.** Both layouts fit in cache at this
size — 10,000 rows is 80 KB of one column against 880 KB of records — so this measures instruction count
more than memory bandwidth. The layout advantage grows precisely where it matters, on corpora too large
to cache, because the column scan touches 8 bytes per row where the row scan pulls an 88-byte record
through cache to read one field of it.

It also is not where most of Phase 15's win should come from. Phase 11 measured per-row filter evaluation
at ~7 ns, most of it constructing a `Value` to compare against, and Phase 12 measured a broad filter as
~12x dominated by materializing and sorting matches. Those costs are in the engine, not the layout;
columns are what make removing them possible.

**Verification costs ~1.4 ms per 10,000 rows**, which is the price of columns having no per-row checksum
— an O(rows) read whenever a root is opened read-write. That is ~140 ms at a million rows and ~1.4 s at
ten million, and it is CRC throughput rather than I/O: the table-driven byte-at-a-time CRC-32 runs at
roughly 360 MB/s here. Two ways out if it ever matters, neither needed yet: a faster CRC (slice-by-8, or
hardware CRC32C), or exploiting the fact that an append-only column's already-verified prefix stays
verified, so only the tail past the last verified point needs rechecking.

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

### Paging, budgets and discovery (Phase 18)

```
== paging and budgets (Phase 18) ==
paging: 100 pages of 100 by offset                      19.735 ms total
paging: the same walk by cursor                          4.038 ms total
  => cursor/offset                                       0.205 x
paging: deepest page by offset                         117.057 us/query
paging: first page (cursor-sized prefix)                21.224 us/query
budget: row scan, no budget                             43.006 us/query
budget: row scan, budget set                            43.205 us/query
  => overhead                                           -0.332 %
discovery: describe_predicates                           0.006 us/call
discovery: describe_corpus                              18.292 us/call
```

A cursor walk of the whole 10,000-row result costs **~4.9x less** than the same walk by offset (4.0 ms
against 19.7 ms; two runs gave 0.169x and 0.205x, so treat the ratio as ~5x rather than a precise figure).

The mechanism is narrower than the ratio suggests, and worth stating so the number is not read as more
than it is. **A cursor page does not skip the selection pass** — every candidate row is filtered on every
page either way. What offset adds is ordering work: it must order the first `offset + limit` rows to know
which ones its page contains, and that prefix grows with depth. The single-page comparison isolates it:
the deepest offset page costs 117 µs against 21 µs for the first, a ~5.6x penalty paid purely for being
deep, which a cursor never pays.

The budget check is **below measurement noise** (43.0 µs against 43.2 µs, and the sign flipped between
runs), which is why it is charged unconditionally rather than behind a fast path.

`describe_predicates` is effectively free — it reads index bucket sizes. `describe_corpus` is one linear
pass over 10,000 assertions at ~18 µs, i.e. ~1.8 ns/row: fine for shaping a query, not for a loop.

### As-of reconstruction (Phase 19)

```
== as-of reconstruction (Phase 19) ==
as-of: columnar scan, no as-of mode                     26.802 us/query
as-of: as_of_commit at the newest id (same rows)        37.024 us/query
  => reconstruction overhead                             1.381 x
as-of: as_of_commit at the midpoint (half the rows)     17.336 us/query
  => versus no as-of                                     0.647 x
as-of: as_of_observed                                   42.352 us/query
  => versus no as-of                                     1.580 x
```

The claim being checked is that reconstruction is a **constant factor, not a search**. The kernel keeps,
per row, the id of the record that closed it, so deriving status as of an earlier point never scans for
that record; the naive alternative is quadratic and would show up here as a large multiple rather than
1.4x.

The three rows measure different things on purpose. At the newest id the same rows are scanned as without
an as-of mode, so the 1.38x is the reconstruction alone — one byte-array materialization pass over the
visible rows. At the midpoint an as-of query is *cheaper* than a present-tense one (0.65x), because the
rows committed after that point are cut off the scan entirely; that is the realistic case, and quoting it
as the headline would flatter the feature, which is why both are here. `as_of_observed` costs more (1.58x)
because it also reads each closing record's `observed_at`, which is a random access into the row array for
every row something closed.
 
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
