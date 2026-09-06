# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

**Read `AGENTS.md` in full before making changes.** It is the authoritative spec for this project: roadmap phase,
architectural principles, assertion semantics, storage rules, testing requirements, and style. This file only
summarizes what's needed to get moving quickly and adds commands; `AGENTS.md` governs when the two disagree.

## What this is

A C++20 embedded "Knowledge Kernel": a bitemporal assertion store with an append-only log as the source of truth.
The primitive is an `Assertion` (subject/predicate/object plus `valid_from`/`valid_to`/`observed_at`, confidence,
status, and `supersedes_id`/`retracts_id` links), not a row or graph edge.

Phases 1–8 of `AGENTS.md`'s "Current Roadmap" are complete (in-memory temporal model → storage engine → persistent
indexes → storage internals → catalog/payload store → provenance + command layer → hypotheses + bounded traversal →
merge/archival). Phase 9 (performance) is deliberately gated: a benchmark harness and baseline exist, but no
optimization work has started. Check `AGENTS.md`'s roadmap before starting work — the project does not jump ahead of
the current phase, and several headings inside `AGENTS.md` still carry stale "current phase" markers, so trust the
per-phase "Current implementation status" blocks and `CHANGELOG.md` over the headings.

## Build & test commands

```bash
cmake -S . -B build              # configure (only needed once / after CMakeLists changes)
cmake --build build              # build kernel lib, examples, mcp_server, benchmarks, and all test binaries
ctest --test-dir build --output-on-failure   # run all tests
```

Each test binary is a plain executable, not a test-framework runner, so there's no `-k`/filter flag — to run one
suite, run it directly (`./build/knowledge_kernel_tests`, `./build/assertion_log_tests`, ...). To run a single test
*function*, comment out the other calls in that file's `main` or add a temporary call; there is no filtering
mechanism.

Other targets: `./build/kernel_demo` (guided tour of the API), `./build/agent_workflow_demo`,
`./build/catalog_usage_example`, and `./build/mcp_server <storage-root>` (stdio JSON-RPC MCP server — test it by
piping newline-delimited JSON-RPC into it; it has no unit tests, like the other entry points).

Benchmarks are `add_executable` targets only — deliberately **not** registered with ctest, since they report
throughput, not pass/fail. Build them in a separate Release tree, since `build/` stays Debug:

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release && cmake --build build-release
./build-release/commit_benchmark   # also query_benchmark, replay_benchmark
```

Per `AGENTS.md`'s Performance Rules, any optimization must first add/update a benchmark and record a baseline in
`docs/benchmarks.md`, then compare after.

There is no lint step; formatting follows `.clang-format` (LLVM style, 4-space indent, 120-column limit).
`compile_commands.json` is generated into `build/` (`CMAKE_EXPORT_COMPILE_COMMANDS` is on) where clangd finds it.

Tests are hand-rolled with `<cassert>` — no gtest/catch2. Follow the existing pattern in `tests/*.cpp`: free
functions named for the behavior under test, called from `main`, using temporary directories (never fixed paths like
`./data` or `/tmp/kernel`) for any storage test so tests stay isolated. Each new `tests/foo_tests.cpp` needs three
entries in `CMakeLists.txt`: `add_executable`, `target_link_libraries(... PRIVATE kernel)`, and `add_test`.

## Architecture

Headers live in `include/kernel/` (so `#include "kernel/knowledge_kernel.hpp"`) but the namespace is `knk::`.
`third_party/nlohmann/json.hpp` is the only external dependency, vendored deliberately.

Layering is strict and one-directional — do not blur it:

```
KnowledgeKernel   query semantics + public commit/query API; owns assertions_, Catalog, provenance_
IndexManager      in-memory indexes, keyed by AssertionId only; returns ids, never Assertions
StorageEngine     coordinates every durable file (owns AssertionLog + the index/catalog/payload logs)
AssertionLog      append/read raw assertion records; no semantics
```

`AssertionLog` and `StorageEngine` must never know about bitemporal semantics, supersession, or "current" state.
`IndexManager` returns `AssertionId`s only — the kernel resolves IDs into `Assertion`s.

Above the kernel sit two thin, non-business-logic layers: `KernelCommand`/`KernelResult` (`kernel_command.hpp`,
`kernel_result.hpp`) reify every public operation, `KnowledgeKernel::execute` dispatches each 1:1 to the mirrored
method, and `mcp_tools.cpp` exposes one MCP tool per command variant over `mcp/main.cpp`'s stdio JSON-RPC loop. Add a
public kernel method → add its command variant, its result handling, and its MCP tool (`mcp_tools_tests.cpp` asserts
every `KernelCommand` variant has exactly one tool).

### Two kinds of durable files — different corruption policies

This distinction is the thing most easily got wrong:

- **Derived state** — the three persisted indexes (`subject.idx`, `current.idx`, `observed_time.idx`), the
  checkpoint, the snapshot, and everything in memory. All rebuildable by replaying `assertions.log`, so corruption is
  caught, the index is discarded, and everything is rebuilt from the log (self-heal).
- **Authoritative metadata logs** — `catalog/entities.log`, `catalog/predicates.log`, `catalog/entity_merges.log`,
  `provenance/provenance.log`, and `payloads/`. `assertions.log` never stores names, values, provenance, or merge
  decisions, so there is nothing to rebuild them from. These get `AssertionLog`'s treatment: non-tail corruption is a
  fatal `std::runtime_error` out of the `KnowledgeKernel` constructor, never caught and never self-healed.

`KnowledgeKernel`'s constructor encodes this: authoritative replay runs first and uncaught, then a fast path (usable
snapshot + all three index files loaded cleanly + checkpoint equals the newest committed id) versus a full-replay
fallback that resets `IndexManager` and `apply()`s every record from id 1. The in-memory object index has no log of
its own and is rebuilt in a linear pass over `assertions_` on the fast path.

### Key invariants

- **The log is the source of truth.** `assertions_`, `IndexManager` state, etc. are derived and must be rebuildable
  by replaying `assertions.log`.
- **Durable-before-visible.** Every commit path appends to the storage engine *before* applying the change in memory
  (`storage_.append_assertion(...)` then `apply(...)`), never the reverse.
- **Replay vs. commit are different code paths.** Replay (`restore_assertion` on the fast path, `apply` on the
  fallback) only ever touches memory and must never write back to the log.
- **Batch commits are prefix-durable, not atomic.** `commit_batch` collapses N assertions into one fsync per log; a
  crash mid-batch leaves the first *k* entries committed — never a gap or a reordering, and ids are consecutive so
  *k* is recoverable. Don't describe it as all-or-none; making it so would need a log format change (see AGENTS.md's
  "Batch Commits"). `commit_batch_by_name` interns *before* that boundary, so a new name is its own fsync.
- **Append-only.** Corrections/removals are new records: `commit_superseding` marks the old assertion `Superseded`,
  `commit_retraction` writes a `Retraction`-status audit record and marks the target `Retracted`. Both validate their
  target ID exists before appending, so a failed call never burns an `AssertionId` or persists an invalid record.
- Normal query methods (`current`, `valid_at`, `known_at`, `valid_at_known_at`, the `*_by_name`/`*_by_object`/
  `*_by_predicate` variants) exclude `Superseded`, `Retracted`, and `Retraction` records; only the timeline/history
  methods (`valid_time_timeline`, `observed_time_timeline`, `commit_history`, `changes_since`) surface full audit
  history. `docs/query_semantics.md` has the per-method table.
- **Single writer, enforced.** `StorageEngine`'s constructor takes an advisory exclusive `flock()` on the storage
  root for its lifetime, so a second open of the same root fails fast. `storage_lock_` is declared before every log
  member so it is acquired first and released last — keep that ordering.
- **MCP schema property order is a wire-format guarantee, not style.** In `mcp_tools.cpp`, declare parameters in
  signature order with all optional ones last, and list `required` as a *prefix* of that order. Everything from
  `ToolSpec::input_schema` to `mcp/main.cpp`'s `dump()` must stay `nlohmann::ordered_json` — a plain `json` re-sorts
  properties alphabetically and silently shifts positionally-bound arguments (see `AGENTS.md`'s "MCP Parameter
  Ordering" and issue #47).

### Error handling and naming

Throw `std::runtime_error` for unrecoverable storage errors, return `std::optional` for a missing assertion, and an
empty vector for a valid empty result. Never silently ignore invalid record sizes, corrupt files, id collisions, or
invalid supersession/retraction targets; incomplete *trailing* log records may be ignored only where documented and
tested. Types use short explicit names (`knk::Assertion`, `knk::KnowledgeKernel`); avoid vague verbs like
`process`/`handle`/`update` — prefer `apply_replayed_assertion`, `mark_superseded`.

## Change conventions

Branches are prefixed by kind (`feature/`, `fix/`, `tests/`, `refactor/`, `docs/`, `chore/`); commit subjects use a
bracketed kind, e.g. `[fix]: emit MCP tool schema properties in declaration order`. Add a `CHANGELOG.md` entry under
`## [Unreleased]` for any user-facing change. Changing on-disk layout means documenting it in
`docs/storage_format.md`, versioning the format, and adding read/write *and* corruption tests. Keep PRs small enough
to read in one sitting and preserve the public API unless the change is about changing it.

## Things to avoid unless explicitly asked

Per `AGENTS.md`'s "Do Not Do Yet": no SQL layer, HTTP API / network transport, LLM extraction *inside* the kernel,
distributed consensus, persistent B-trees, general graph traversal, RDF/ontology support, custom allocators,
compression, or replication. Two narrowings, not liftings: the capped-hop `neighbors`/`co_occurring_predicates` are
the only allowed graph traversal, and the local stdio MCP server is the only allowed caller-facing boundary (an
external agent writing already-structured assertions through the public API was always in scope). No SIMD, mmap,
lock-free structures, or other Phase 9 performance work before the correctness model is stable — and only
benchmark-driven when it starts.
