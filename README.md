# knk — Knowledge Kernel

[![CI](https://github.com/cs0lar/knk/actions/workflows/ci.yml/badge.svg)](https://github.com/cs0lar/knk/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)

A C++20 embedded **bitemporal assertion store** with an append-only log as the source of truth —
the persistent core for conversational, temporal, and provenance-aware knowledge systems.

## What it is

The primitive isn't a row, a document, or a graph edge. It's an **`Assertion`**: a versioned,
temporally-scoped claim about the world —

```text
subject --predicate--> object
```

carrying `valid_from`/`valid_to` (when it's true in the world), `observed_at` (when the kernel
learned it), `confidence`, `status`, and `supersedes_id`/`retracts_id` links back to whatever it
corrected or withdrew. The assertion log is append-only: nothing is ever overwritten in place.
Corrections and removals are new records that supersede or retract earlier ones, so the kernel
never loses the history of how its knowledge changed.

## Key capabilities

- **Bitemporal queries** — `current`, `valid_at`, `known_at`, `valid_at_known_at`, and full
  valid-time/observed-time/commit-history timelines per subject/predicate.
- **Durable, recoverable storage** — a segmented, checksummed, fsync'd append-only log with
  crash-tolerant recovery, persistent indexes, and explicit snapshots for fast restart.
- **Batch commits** — `commit_batch` appends many assertions under a single durability boundary
  (one fsync per log for the whole batch, not per assertion), each entry keeping its own valid
  time, returning the new ids in input order.
- **Entity/predicate catalog** — idempotent name and typed-literal interning
  (`intern_entity`/`intern_value`/`intern_predicate`), plus a payload store for large content
  (documents) addressed by id.
- **Provenance & audit** — `record_provenance`/`provenance_for` ("which source produced this
  claim?"), `explain` (walk a supersession/retraction chain to its root), and `find_conflicts`
  (overlapping active assertions for the same subject/predicate).
- **Hypotheses & bounded graph traversal** — a labeled `Hypothesis` status for machine-suggested
  facts, plus small, capped-hop `neighbors`/`co_occurring_predicates` feature extraction for an
  external prediction/causal-inference tool.
- **Entity merge & archival** — one-way deduplication redirects (`merge_entities`) and segment
  archival/compaction (`archive_segments_before`) — pruning, never deletion.
- **A closed command layer + local MCP server** — every public operation is reified as a
  `KernelCommand`/`KernelResult` pair, and a stdio [Model Context
  Protocol](https://modelcontextprotocol.io/) server (`mcp_server`) exposes each one as a tool, so
  an external agent process can call into the kernel without linking against the C++ API.

See [`AGENTS.md`](AGENTS.md) for the full design spec, architectural invariants, and roadmap this
project follows.

## Quick start

```bash
cmake -S . -B build              # configure
cmake --build build              # build the kernel lib, kernel_demo, mcp_server, and all tests
ctest --test-dir build --output-on-failure   # run the test suite
```

Each test binary is also a plain executable you can run directly, e.g. `./build/knowledge_kernel_tests`.

## A minimal example

```cpp
#include "kernel/knowledge_kernel.hpp"

using namespace knk;

KnowledgeKernel kernel(StorageConfig{"/path/to/storage"});

// Commit a fact from names/literals -- interning is idempotent, so this is safe to call again.
kernel.commit_by_name("Alice", "works_at", Value::of_text("Acme"),
                       /*valid_from=*/1704067200, /*valid_to=*/OPEN_ENDED,
                       /*observed_at=*/1719792000, /*confidence=*/0.95);

// Ask what we currently know about Alice, by name.
for (const Assertion &fact : kernel.current_by_name("Alice")) {
    // fact.subject / fact.predicate / fact.object are EntityId/PredicateId/EntityId
}
```

`examples/knowledge_kernel_demo.cpp` is a guided, runnable tour of the full API (`./build/kernel_demo`);
`examples/agent_workflow_demo.cpp` chains several features together the way a real caller would.

## Architecture

Layering is strict and one-directional:

```text
KnowledgeKernel   query semantics + public commit/query API
IndexManager      in-memory and persistent indexes, keyed by AssertionId only
StorageEngine     coordinates durable storage (owns AssertionLog)
AssertionLog      append/read raw assertion records; no semantics
```

`AssertionLog`/`StorageEngine` never know about bitemporal semantics or "current" state;
`IndexManager` returns ids only, never full assertions — the kernel resolves them. The log is
always the source of truth: every other in-memory structure is rebuildable by replaying it.

## MCP server

```bash
./build/mcp_server <storage-root>
```

A subprocess speaking newline-delimited JSON-RPC 2.0 over its own stdin/stdout — one MCP tool per
`KnowledgeKernel` operation. See [`docs/mcp_server.md`](docs/mcp_server.md) for the full tool list,
an example JSON-RPC session, and how to point a real MCP client at it.

## Project status

Phases 1–8 of the roadmap in [`AGENTS.md`](AGENTS.md#current-roadmap) are complete: in-memory
temporal model, storage engine, persistent indexes, segmented/checksummed storage internals,
entity/predicate catalog + payload store, provenance + the command layer, hypotheses + bounded
graph traversal, and entity merge + archival. Phase 9 (performance work — SIMD, mmap, lock-free
readers, etc.) is explicitly gated behind a demonstrated bottleneck from real usage; a benchmark
harness and baseline exist (see [`docs/benchmarks.md`](docs/benchmarks.md)), but no optimization
work has started.

## Further reading

- [`AGENTS.md`](AGENTS.md) — the authoritative spec: architecture, assertion semantics, storage
  rules, roadmap, testing requirements, and style.
- [`docs/query_semantics.md`](docs/query_semantics.md) - what each query method returns and excludes.
- [`docs/storage_format.md`](docs/storage_format.md) — on-disk format and durability model.
- [`docs/mcp_server.md`](docs/mcp_server.md) — MCP tool reference and protocol usage.
- [`docs/benchmarks.md`](docs/benchmarks.md) — benchmark methodology and baseline numbers.
- [`CONTRIBUTING.md`](CONTRIBUTING.md) — how to propose a change.

## License

[MIT](LICENSE)
