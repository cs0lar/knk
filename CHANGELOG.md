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
