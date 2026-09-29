# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/). This project has
not yet cut a tagged release, so everything so far is tracked under `Unreleased`; once versioning
starts it will follow [Semantic Versioning](https://semver.org/).

Going forward, please add an entry here (under `Unreleased`) for any user-facing change as part of
the PR that makes it — see `CONTRIBUTING.md`.

## [Unreleased]

### Added

- **Phase 1 — In-memory temporal database:** the `Assertion` model, bitemporal commit/query
  semantics, and superseded/retracted status handling.
- **Phase 2 — Storage engine:** `AssertionLog`/`StorageEngine`, durable-before-visible commit
  ordering, and full recovery via log replay on startup.
- **Phase 3 — Persistent indexes:** durable subject, current-state, predicate, and observed-time
  indexes with corruption self-heal and full-replay fallback.
- **Phase 4 — Storage engine internals:** segmented log files, per-record CRC32 checksums,
  fsync'd durability, an index checkpoint closing the assertion/index cross-log atomicity gap, and
  explicit full-state snapshots for fast restart.
- **Phase 5 — Entity/predicate catalog and payload store:** idempotent name/value interning
  (`intern_entity`/`intern_value`/`intern_predicate`), a `PayloadStore` for large content, and the
  `commit_by_name`/`current_by_name` name-based convenience overloads for the write and read paths.
- **Phase 6 — Provenance & agentic interface:** `record_provenance`/`provenance_for`, `explain`
  (walk a supersession/retraction chain to its root), `find_conflicts` (overlapping active
  assertions), and the closed `KernelCommand`/`KernelResult` reified command layer.
- **Phase 7 — Anticipatory layer:** `AssertionStatus::Hypothesis`, `commit_hypothesis`/
  `hypotheses_for` for machine-suggested facts, and bounded local graph traversal (`neighbors`,
  `co_occurring_predicates`).
- **Phase 8 — Self-improvement: merge & prune:** one-way entity-merge redirects (`merge_entities`/
  `resolve_entity`) and segment archival/compaction (`archive_segments_before`) — pruning, never
  deletion.
- **Phase 9 — Performance (in progress):** a benchmark harness (`benchmarks/`) and recorded
  baseline numbers; no optimization work has started yet — it's gated behind a demonstrated
  bottleneck, per `AGENTS.md`'s Performance Rules.
- A local stdio [MCP](https://modelcontextprotocol.io/) server (`mcp_server`) exposing every
  `KernelCommand` as a discoverable tool, plus `docs/mcp_server.md` documenting the protocol and
  full tool list.
- Public-repo scaffolding: `README.md`, `LICENSE` (MIT), `CONTRIBUTING.md`, `CODE_OF_CONDUCT.md`,
  `SECURITY.md`, and CI (GitHub Actions build + test on push/PR).

### Added

- `StorageEngine` now takes an advisory exclusive lock (POSIX `flock()`) on the storage root for
  its lifetime, enforcing the single-writer model `AGENTS.md`'s Concurrency Rules already declared
  but nothing previously checked. A second concurrent open of the same root — via `mcp_server` or
  any direct `libkernel.a` caller — now fails fast with a clear error naming the path instead of
  silently corrupting the log; the lock is released automatically on process exit, including
  `SIGKILL`, with no manual cleanup required. See `docs/storage_format.md`'s "Storage root lock"
  section.

- `commit_batch` commits many new active assertions in a single call under one durability boundary:
  one fsync per underlying log for the whole batch rather than one per assertion (~67x the
  throughput of the equivalent `commit()` loop on the machine measured in `docs/benchmarks.md`, and
  a larger factor the slower `fsync` is). Each entry carries its own `valid_from`/`valid_to`/
  `observed_at`, so restating a field across a whole population preserves per-record valid time, and
  the new ids come back in input order so a caller can attach provenance without a lookup per
  assertion. Batches are capped at `KnowledgeKernel::MAX_BATCH_SIZE` (10,000); an over-sized batch is
  rejected before anything is written and burns no `AssertionId`. Durability is prefix-shaped rather
  than atomic — a crash mid-batch leaves the first *k* entries committed, never a gap or a
  reordering, and consecutive ids make *k* exactly recoverable. Available as a `CommitBatchCommand`
  and as the `commit_batch` MCP tool. Plain appends only: corrections still go through
  `commit_superseding`/`commit_retraction`.

- `commit_batch_by_name` is `commit_batch`'s name-based overload, standing to it as `commit_by_name`
  does to `commit`: it interns each entry's subject name, predicate name, and object `Value` (all
  idempotent), then commits the resolved entries through `commit_batch`, so every batch guarantee
  carries over. Note that interning happens *before* the batch's durability boundary, so a genuinely
  new name costs its own durable write — the intended case, restating a field for subjects already in
  the catalog, interns nothing new for the subjects and at most one new predicate. An over-sized batch
  is rejected before any interning, leaving no catalog entries behind either. Available as a
  `CommitBatchByNameCommand` and as the `commit_batch_by_name` MCP tool.

- `record_provenance_batch` records provenance for many assertions under a single durability
  boundary — one fsync on `provenance.log` for the whole list, not one per record. This was the last
  place per-assertion fsync cost survived on the batch path: attaching provenance one record at a
  time cost 8.5x the `commit_batch` it described (82 ms vs ~7-10 ms for 10,000 assertions); batched
  it is ~53x faster (see `docs/benchmarks.md`). `commit_batch` returns its ids in input order for
  exactly this, so a caller can zip them with sources without a lookup per assertion. Every target is
  validated before anything is appended, so one unknown id rejects the whole call without writing a
  record. Deliberately a separate call rather than per-entry provenance inside `commit_batch`, which
  could leave provenance referencing assertions a torn batch never committed. Bounded by
  `MAX_BATCH_SIZE`; available as a `RecordProvenanceBatchCommand` and the `record_provenance_batch`
  MCP tool.

- **Phase 13 — read-only concurrent opens:** `KnowledgeKernel` and `StorageEngine` take an `OpenMode`,
  and `mcp_server <root> --read-only` exposes it, so an external process can query a storage root while
  another process is writing it — previously impossible, since every open took the exclusive writer
  lock. A read-only open takes **no** lock (a shared lock on the writer's own lock file can never be
  acquired while the writer holds it), creates nothing (not even the root: a read-only open of a missing
  root throws rather than conjuring one), and writes nothing — every mutating method throws, guarded at
  both the kernel and storage layers, and over MCP a refused write is an ordinary tool error rather than
  a crash. Two limits worth knowing: a read-only kernel is a **snapshot as of its own construction**,
  not a live view, so later commits need a reopen; and it cannot repair derived state, so a root with a
  stale or corrupt index is rebuilt in memory and answered correctly from the log while the files are
  left for a writer to heal. Single-writer enforcement is unchanged: a second writer still fails fast.

- **Phase 12 — aggregation and grouping:** `aggregate` answers `count`, `count_distinct`, `sum`,
  `min`, `max` and `avg` over matching assertions, optionally grouped by subject, predicate, object,
  status, or a fixed-width valid-time or observed-time bucket. Targets include the object's *interned
  value*, so "average salary by department" is now a single call rather than a fetch-and-compute. The
  selection half is an ordinary `Query` — same filters, selectors and bitemporal/status rules — so an
  aggregate and a row query can never disagree about which rows are current. Rows with nothing numeric
  to read are skipped rather than counted as zero, with each group reporting `row_count` alongside each
  cell's own count so "10 rows, average over 3" stays visible; a sum over nothing is `null`, not `0`.
  Aggregation streams: rows are folded into their group and dropped, so memory is bounded by groups
  rather than matching rows, and counting every row measures ~11x cheaper than the row query over the
  same rows. Exceeding the 10,000-group cap is an error rather than a silently truncated answer.
  Available as `AggregateCommand` and the `aggregate` MCP tool.

- **Phase 11 — filters, projection and index selection:** a `Query` now carries a `filter` tree —
  comparisons (`eq|ne|lt|lte|gt|gte`) over the ids, `confidence`, the timestamps, `status`, and
  `object_value` (the object's *interned value*, so "salary >= 100000" is expressible), composed with
  `and`/`or`/`not` up to 8 deep. Structurally broken filters are rejected with an error, while a
  comparison meeting a row whose object value is of another kind simply does not match, since objects
  are a mix of named entities and typed literals by design. `resolve_names` returns catalog names and
  object values alongside the returned page, so rendering query results no longer needs a second round
  trip through the batch resolvers. Index selection now uses the observed-time index for a subject
  query with an `observed_to` bound and picks the smaller bucket when a current-shaped query names both
  an object and a predicate — cost only: a new randomized differential suite answers 2,000 seeded
  queries three ways (index-selected, forced-scan, and an independently written brute-force evaluator)
  and requires all three to agree. Also fixes two Phase 10 defects: an empty index bucket no longer
  triggers a full rescan, and `QueryEngine` no longer holds references into `KnowledgeKernel`, which
  had made the kernel unsafe to move.

- **Phase 10 — query IR and executor parity:** `KnowledgeKernel::query(const Query&)` answers a shaped
  read — optional subject/predicate/object, a valid-time point, an inclusive observed-time window,
  `open_ended_only`, an explicit status set, deterministic ordering, and `limit`/`offset` paging —
  executed by a new `QueryEngine` that holds the only place query semantics compose. Every existing
  query method is expressible as a `Query` returning identical rows, asserted by parity tests rather
  than assumed. Deliberately a reified, typed IR rather than SQL text: no parser, no dialect, and a
  JSON Schema an MCP client can discover. Results are bounded (`limit == 0` means the 10,000-row
  ceiling, larger limits are capped, and `QueryResult::truncated` reports that more matched) and the IR
  is versioned, so an unknown `ir_version` is rejected rather than reinterpreted. Available as
  `QueryCommand` and the `query` MCP tool. The existing query methods are unchanged and remain the
  documented way to ask the simple questions.

- Batch reads (#55): `entity_name_batch`, `entity_value_batch`, `predicate_name_batch`, and
  `provenance_for_batch` resolve many ids in one call, the read-side counterparts of the write
  batches. Reads like `current_by_predicate` return records whose fields are ids, so a caller
  rendering them used to pay one MCP round trip per id; these make that one call. Answers come back
  in input order, one slot per id, each slot exactly what the single resolver answers for that id —
  including `null` for an id that was never interned, for `entity_name` on a non-text literal, and
  for an assertion with no recorded provenance. Unknown ids are therefore `null` slots rather than a
  rejected call, matching the single resolvers, which never throw for one. Bounded by
  `MAX_BATCH_SIZE`, checked before anything is read. Available as `*BatchCommand`s and as MCP tools;
  the single-id resolvers are unchanged.

- `changes_since` gained optional `limit` and `newest_first` parameters, and `assertions_for_subject`/
  `commit_history` gained an optional `limit` — all default to prior (unlimited, oldest-first)
  behavior. Lets a caller answer "what's the single latest change" via
  `changes_since(0, limit=1, newest_first=true)` instead of reading and discarding the entire log to
  find the tail.

### Fixed

- `mcp_server`'s `initialize` response advertised the original `2024-11-05` MCP protocol revision;
  bumped to `2025-06-18` so newer clients that reject stale revisions can connect.
- `mcp_server` no longer crashes with an unhandled-exception abort if the storage root can't be
  opened (e.g. it's already locked by another process); it now prints a clear error to stderr and
  exits with status 1.
- MCP tool schemas now emit `properties` in the order they're declared, with `required` as a
  prefix of that order, as `tools/list` sends them over the wire. `ToolSpec::input_schema` was
  `nlohmann::json`, whose default object silently re-sorts keys alphabetically on serialization;
  this was invisible until #43 added the first optional trailing parameters (`limit`/
  `newest_first` on `assertions_for_subject`/`commit_history`/`changes_since`), at which point the
  serialized schemas advertised those optional parameters *ahead* of the required ones — breaking
  any caller (e.g. treelang) that binds arguments positionally against the emitted order. Fixed by
  switching the schema-building path to `nlohmann::ordered_json` end-to-end. See #47 and
  `AGENTS.md`'s new "MCP Parameter Ordering" section.
