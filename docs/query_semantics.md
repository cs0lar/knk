# Query semantics

This document describes what each public `KnowledgeKernel` query method returns and, just as importantly, what it excludes.

## The key rule

**Normal query methods only surface `AssertionStatus::Active` facts** (and, for hypothesis helpers, `Hypothesis`). They **exclude**:

- `Superseded` — replaced by a later assertion
- `Retracted` — withdrawn knowledge
- `Retraction` — the audit record that performed a retraction

**Timeline / history methods are the exception.** They exist so callers can reconstruct *how* knowledge changed. `commit_history` is status-agnostic (full audit trail for a subject/predicate). The valid-time and observed-time timeline helpers still filter to `Active` assertions for a given predicate, sorted for presentation — use `commit_history` or `explain` when you need superseded/retracted/retraction rows.

`get(id)` is also status-agnostic: it returns the stored record for any known id, including non-active statuses, so audit tooling can inspect a specific assertion.

Entity ids passed as *subjects* (and, where noted, *objects*) are resolved through `Catalog::resolve` first, so a caller holding an id that was later merged still sees results for the surviving id.

---

## Current-state queries

These answer "what does the kernel believe **right now**?" They use the current-state index of active, open-ended assertions.

### `current(subject)`

Returns every currently **Active**, open-ended assertion where `subject` is the subject.

**Excludes:** Superseded, Retracted, Retraction, Hypothesis, and any assertion whose valid interval is closed.

### `current_by_name(subject_name)`

Resolves `subject_name` with `find_entity` (lookup only — **never interns** a missing name) and delegates to `current`. Returns an empty vector if the name was never interned.

**Excludes:** same as `current`, plus "unknown name".

### `current_by_object(object)`

Reverse of `current(subject)`: every currently **Active**, open-ended assertion where the given entity is the **object** (e.g. "who works at Acme?"). `object` is catalog-resolved.

**Excludes:** same status set as `current`.

### `current_by_predicate(predicate)`

Kernel-wide: every currently **Active**, open-ended assertion for the given predicate, any subject (e.g. every `WORKS_AT`).

**Excludes:** same status set as `current`. No entity resolution (predicates are not merge-redirected).

---

## Temporal queries

These answer "what was true / known at time *t*?" They scan the subject index and keep only **Active** assertions that satisfy the time predicate.

### `valid_at(subject, valid_time)`

Assertions for `subject` that are **Active** and whose valid interval covers `valid_time` (half-open `[valid_from, valid_to)`, with `OPEN_ENDED` unbounded).

**Excludes:** Superseded, Retracted, Retraction, Hypothesis; Active assertions outside the interval.

### `known_at(subject, observed_time)`

Assertions for `subject` that are **Active** and were observed at or before `observed_time` (`observed_at <= observed_time`), using the observed-time index.

**Excludes:** non-Active statuses; assertions observed after `observed_time`.

### `valid_at_known_at(subject, valid_time, observed_time)`

Bitemporal intersection: assertions known by `observed_time` whose valid interval covers `valid_time`, **Active** only.

**Excludes:** non-Active statuses; anything outside either time bound.

---

## Timeline and history (audit)

### `valid_time_timeline(subject, predicate)`

**Active** assertions for `(subject, predicate)`, sorted by valid time. Presentation order for the live timeline — **not** full audit history.

**Excludes:** Superseded, Retracted, Retraction, Hypothesis; other predicates.

### `observed_time_timeline(subject, predicate)`

**Active** assertions for `(subject, predicate)`, sorted by observed time.

**Excludes:** same as `valid_time_timeline`.

### `commit_history(subject, predicate, limit = 0)`

**Status-agnostic** audit list: every stored assertion for `(subject, predicate)`, including Superseded, Retracted, and Retraction records, sorted by assertion id (commit order). Optional `limit` caps the prefix after sort (`0` = unlimited).

**Includes:** full audit history for that subject/predicate. This is the primary exception to the "exclude non-Active" rule.

---

## Other query helpers

### `assertions_for_subject(subject, limit = 0)`

Assertions indexed under `subject` (after catalog resolve). Optional `limit` is a prefix cap on the existing order, not "N most recent". Prefer the specialized query methods above when you care about status filtering.

### `hypotheses_for(subject)`

Mirrors `current` but selects **`Hypothesis`** status only (hypotheses are never on the current-state index).

**Excludes:** Active, Superseded, Retracted, Retraction.

### `changes_since(observed_since, limit = 0, newest_first = false)`

Kernel-wide "what changed since *t*": every assertion with `observed_at >= observed_since`, **any status**, any subject/predicate. Sorted by `observed_at` then id (or reversed when `newest_first`). Optional `limit` after sort.

**Includes:** new commits, supersessions, retractions, hypotheses — full change stream.

### `explain(id)`

Walks the supersession/retraction chain from `id` back to the root (`supersedes_id` / `retracts_id`), newest-first. Status-agnostic. Empty if id unknown/zero.

### `find_conflicts(subject, predicate)`

Unordered pairs of **Active** assertions for the same subject/predicate with different objects whose valid intervals overlap. Superseded/retracted rows are never reported as conflicts.

---

## Quick reference

| Method | Active only? | Notes |
| --- | --- | --- |
| `current` / `current_by_*` | Yes | Open-ended current facts |
| `valid_at` / `known_at` / `valid_at_known_at` | Yes | Temporal slice |
| `valid_time_timeline` / `observed_time_timeline` | Yes | Sorted Active timeline |
| `commit_history` | **No** | Full audit for subject+predicate |
| `changes_since` | **No** | Global change stream |
| `explain` | **No** | Chain walk by id |
| `get` | **No** | Single record by id |
| `hypotheses_for` | Hypothesis only | Not Active |
| `find_conflicts` | Yes (pairs) | Overlapping Active facts |

When in doubt: if you need history, use `commit_history`, `changes_since`, `explain`, or `get`. If you need "what is true now / at time t", use the current and temporal queries and expect non-Active statuses to be invisible.
