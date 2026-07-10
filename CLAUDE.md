# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

**Read `AGENTS.md` in full before making changes.** It is the authoritative spec for this project: roadmap phase,
architectural principles, assertion semantics, storage rules, testing requirements, and style. This file only
summarizes what's needed to get moving quickly and adds commands; `AGENTS.md` governs when the two disagree.

## What this is

A C++20 embedded "Knowledge Kernel": a bitemporal assertion store with an append-only log as the source of truth.
The primitive is an `Assertion` (subject/predicate/object plus `valid_from`/`valid_to`/`observed_at`, confidence,
status, and `supersedes_id`/`retracts_id` links), not a row or graph edge. Currently in Phase 2 (storage engine) of
the roadmap defined in `AGENTS.md` — check that file's "Current Roadmap" section before starting work, since it says
not to jump ahead of the current phase.

## Build & test commands

```bash
cmake -S . -B build              # configure (only needed once / after CMakeLists changes)
cmake --build build              # build kernel lib, kernel_demo, and all test binaries
ctest --test-dir build --output-on-failure   # run all tests
```

Run a single test binary directly (each is a plain executable, not a test-framework runner, so there's no `-k`/filter flag):

```bash
./build/knowledge_kernel_tests
./build/assertion_log_tests
./build/index_manager_tests
```

There is no separate lint step; formatting follows `.clang-format` (LLVM style, 4-space indent, 120 col limit) and
`compile_commands.json` is symlinked at the repo root for clangd.

Tests are hand-rolled with `<cassert>` — no gtest/catch2. When adding a test, follow the existing pattern in
`tests/*.cpp`: free functions named for the behavior under test, called from `main`, using temporary directories
(never fixed paths like `./data` or `/tmp/kernel`) for any storage engine test so tests stay isolated.

## Architecture

Layering is strict and one-directional — do not blur it:

```
KnowledgeKernel   query semantics + public commit/query API
IndexManager      in-memory (later persistent) indexes, keyed by AssertionId only
StorageEngine     coordinates durable storage (owns AssertionLog)
AssertionLog      append/read raw assertion records; no semantics
```

`AssertionLog` and `StorageEngine` must never know about bitemporal semantics, supersession, or "current" state.
`IndexManager` returns `AssertionId`s only, never full `Assertion`s — the kernel resolves IDs.

Key invariants (see `AGENTS.md` for the full rationale):

- **The log is the source of truth.** In-memory `assertions_`, `IndexManager` state, etc. are all derived and must be
  rebuildable by replaying `assertions.log`.
- **Durable-before-visible.** Every commit path appends to the storage engine *before* applying the change to
  in-memory state (`storage_.append_assertion(...)` then `apply_assertion(...)`), never the reverse.
- **Replay vs. commit are different code paths.** Replay (constructor calls `apply_replayed_assertion`) only ever
  applies to memory and must never write back to the log.
- **Append-only.** Corrections/removals are new records: `commit_superseding` marks the old assertion `Superseded`,
  `commit_retraction` writes a `Retraction`-status audit record and marks the target `Retracted`. Both validate their
  target ID exists before appending, so a failed call never burns an `AssertionId` or persists an invalid record.
- Normal query methods (`current`, `valid_at`, `known_at`, `valid_at_known_at`) exclude `Superseded`, `Retracted`, and
  `Retraction` records; only the timeline/history methods (`valid_time_timeline`, `observed_time_timeline`,
  `commit_history`) surface full audit history.

Namespace is `knk::`; types use short explicit names (`knk::Assertion`, `knk::KnowledgeKernel`, ...). Avoid vague
verbs like `process`/`handle`/`update` — prefer names like `apply_replayed_assertion`, `mark_superseded`.

## Things to avoid unless explicitly asked

Per `AGENTS.md`'s "Do Not Do Yet": no SQL layer, HTTP API, LLM extraction, distributed consensus, persistent
B-trees, RDF/ontology support, custom allocators, compression, or replication. Also no SIMD, mmap, lock-free
structures, or other Phase 5 performance work before the correctness model (through Phase 4) is stable — that work
must be benchmark-driven (add/update a benchmark, record a baseline, then optimize).
