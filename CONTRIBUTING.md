# Contributing to knk

Thanks for considering a contribution. This project follows a fairly strict architectural and
process discipline — reading a little first saves everyone a round trip on review.

## Start here

**[`AGENTS.md`](AGENTS.md) is the authoritative spec.** It defines the current roadmap phase,
architectural invariants (log-is-source-of-truth, durable-before-visible, append-only, strict
layering), assertion/query semantics, storage rules, testing requirements, and style. Read the
"Current Roadmap" section before starting work — the project deliberately does not jump ahead of
the current phase, and PRs that do will be asked to narrow scope.

## Build & test

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Each test binary is also a plain executable (no test-framework filter flag), e.g.
`./build/knowledge_kernel_tests`. There's no separate lint step; formatting follows
`.clang-format` (LLVM style, 4-space indent, 120-column limit).

## Workflow

1. Branch off `main`. This repo's branch names are prefixed by kind:
   `feature/…`, `fix/…`, `tests/…`, `refactor/…`, `docs/…`, `chore/…`.
2. Keep changes small — a good PR should be understandable in one sitting, and should touch one
   roadmap phase or concern at a time.
3. Preserve the existing public API unless the change is explicitly about changing it.
4. Add or update tests for anything you change. This project uses hand-rolled `<cassert>`-based
   tests (no gtest/catch2) — follow the existing pattern in `tests/*.cpp`: free functions named for
   the behavior under test, called from `main`, using temporary directories (never fixed paths like
   `./data`) for any storage-engine test so tests stay isolated.
5. Run the build and full test suite locally before opening a PR.
6. Add a `CHANGELOG.md` entry under `## [Unreleased]` for any user-facing change.
7. Open the PR, explain what changed and why, and call out any tradeoffs or unfinished work.

Commit messages in this repo use a bracketed-kind prefix, e.g. `[feature]: …`, `[fix]: …`,
`[docs]: …`, `[tests]: …` — match that style.

## Architecture ground rules

These are load-bearing invariants, not style preferences — see `AGENTS.md` for the full rationale:

- **The log is the source of truth.** In-memory state and indexes are derived and must be
  rebuildable by replaying the assertion log.
- **Durable-before-visible.** Every commit path appends to storage *before* applying the change to
  in-memory state.
- **Append-only.** Corrections and removals are new records (supersession/retraction), never
  in-place mutation of an existing one.
- **Strict, one-directional layering:** `KnowledgeKernel` → `IndexManager` → `StorageEngine` →
  `AssertionLog`. Lower layers must never know about bitemporal semantics or "current" state.

## Scope

Before proposing something outside the current roadmap phase (an HTTP API, SQL layer, LLM
extraction inside the kernel, distributed consensus, compression, etc.), check `AGENTS.md`'s "Do
Not Do Yet" section — several of these are deliberate, explicit non-goals for now, not oversights.

## Code of Conduct

This project follows the [Contributor Covenant](CODE_OF_CONDUCT.md). Report unacceptable behavior
per the instructions there.
