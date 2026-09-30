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

## The query IR (Phase 10)

`KnowledgeKernel::query(const Query&)` answers a *shaped* read: several filters at once, an explicit
status set, deterministic ordering, and paging. It is not a second interpretation of the rules above —
every method on this page is expressible as a `Query` returning identical rows, which
`tests/query_engine_tests.cpp` asserts rather than assumes.

| Method | Equivalent `Query` |
|---|---|
| `current(S)` | `subject=S, statuses={Active}, open_ended_only=true` |
| `current_by_object(O)` | `object=O, statuses={Active}, open_ended_only=true` |
| `current_by_predicate(P)` | `predicate=P, statuses={Active}, open_ended_only=true` |
| `valid_at(S, t)` | `subject=S, statuses={Active}, valid_at=t` |
| `known_at(S, t)` | `subject=S, statuses={Active}, observed_to=t` |
| `valid_at_known_at(S, v, o)` | `subject=S, statuses={Active}, valid_at=v, observed_to=o` |
| `hypotheses_for(S)` | `subject=S, statuses={Hypothesis}` |
| `assertions_for_subject(S, n)` | `subject=S, limit=n` |
| `changes_since(t, n, newest)` | `observed_from=t, order=ObservedAt, newest_first=newest, limit=n` |

Notes that matter when comparing the two:

* **Ordering.** The IR always has one deterministic order: the requested key (`assertion_id`,
  `valid_from`, or `observed_at`), then `AssertionId` as tie-break, reversed wholesale by
  `newest_first`. The methods above mostly do *not*: `current`, `valid_at`, `known_at`,
  `hypotheses_for` and `current_by_*` iterate unordered containers, so their row order is unspecified
  and can differ between runs. Equivalence with those is set equivalence, not sequence equivalence.
  `assertions_for_subject` (subject-index append order, i.e. id-ascending) and `changes_since`
  (`observed_at` then id) do specify an order, and the IR matches it exactly.
* **Status is explicit.** An empty status set means *every* status, matching the audit-shaped reads. A
  query that wants only live facts must say `{Active}`; nothing is excluded implicitly.
* **`open_ended_only` is what makes "current" current.** `statuses={Active}` plus `open_ended_only` is
  precisely `is_current_assertion`, which is what the current-state index stores.
* **Merge redirects** are followed for `subject` and `object` (not `predicate`, which is never merged),
  resolving the query's argument and comparing it against the stored id — exactly what the methods do.
* **Bounded.** `limit == 0` means `MAX_QUERY_RESULT` (10,000), a larger limit is capped to it, and
  `QueryResult::truncated` distinguishes "that was all" from "here is the first page".
* **Versioned.** A `Query` carrying an unknown `ir_version` is rejected, never reinterpreted.
* **Paging** is either `offset` or a `cursor`, never both — see below.

### Filters (Phase 11)

The selectors above are equality on one id. A `Query::filter` is everything else — ordered
comparisons, comparisons against the object's *value* rather than its id, and boolean combinations:

```text
comparison   field op operand        e.g. confidence >= 0.9
and / or     one or more children
not          exactly one child
```

Fields and the operand kind each expects:

| Field | Operand kind | Compares against |
|---|---|---|
| `subject`, `predicate`, `object` | `int64` | the stored id |
| `object_value` | any | the object's interned `Value` |
| `confidence` | `double` | `confidence` |
| `valid_from`, `valid_to`, `observed_at` | `timestamp` | that field (`valid_to == 0` means open-ended) |
| `status` | `text` | the status name, e.g. `"Active"` |

Ordering is defined for every kind: numerically for `int64`/`double`/`timestamp`, lexicographically for
`text`, and `false < true` for `bool`.

The distinction that matters most here is **structural errors throw, heterogeneous data does not
match**:

* An operand of the wrong kind for its field, an unknown status name, an empty `and`/`or`, a `not`
  without exactly one child, or nesting deeper than `MAX_FILTER_DEPTH` (8) is a caller mistake and is
  rejected with an error. A filter that could never match anything is worth saying out loud.
* An `object_value` comparison against a row whose object value is of a *different* kind — or whose
  object has no interned value at all — is simply false for that row. Objects across the kernel are a
  mix of named entities and typed literals, so "object value > 100" meeting a text object is ordinary,
  not an error.

Filters are evaluated per row and never change which index is selected: selectors are the pushdown
surface, filters are not. That keeps "what it returns" and "how fast it runs" independently reviewable,
which is what the randomized differential tests in `tests/query_differential_tests.cpp` check — every
generated query is answered with index selection, with `force_scan`, and by an independent brute-force
evaluator, and all three must agree.

### Name resolution (Phase 11)

`Query::resolve_names` adds a `names` array parallel to the returned rows, each entry carrying the
subject's name (absent if the subject is not a text entity), the predicate's name, and the object's
interned `Value` (absent if it was never interned). It exists so a caller rendering query results does
not need a second round trip through `entity_name_batch`/`predicate_name_batch`/`entity_value_batch`.

Only the returned **page** is resolved, never the whole match set: a 10,000-row match paged three at a
time costs three lookups per field, not 10,000. It is off by default because it costs two catalog
lookups per returned row (~27% on the shape measured in `docs/benchmarks.md`).

## Aggregation (Phase 12)

`KnowledgeKernel::aggregate(const AggregateQuery&)` folds matching rows into groups instead of
returning them. The selection half is an ordinary `Query` — same selectors, same filter tree, same
bitemporal and status rules — so an aggregate and a row query can never disagree about which rows are
current.

| | |
|---|---|
| Functions | `count`, `count_distinct`, `sum`, `min`, `max`, `avg` |
| Targets | `object_value` (the object's interned `Value`), `confidence`, `valid_from`, `valid_to`, `observed_at`, `subject`, `predicate`, `object` |
| Group by | `subject`, `predicate`, `object`, `status`, or a fixed-width `valid_from`/`observed_at` bucket — up to `MAX_GROUP_BY_FIELDS` (4), or none for one global group |

Five behaviors worth knowing before reading a result:

* **Rows with no number are skipped, not counted as zero.** `avg` over a group whose objects are text
  has nothing to average, and treating those as 0 would silently drag every average down. This is why
  each group reports `row_count` *and* each cell reports its own count: 10 rows with an average over 3
  is a representable, visible state.
* **An empty aggregate is null, not zero.** `sum` over no contributing rows returns null, because 0.0
  would be a claim about data that was never there.
* **Counts are integers, everything else is a number.** Counts never pass through a double, so a large
  count cannot lose precision.
* **Buckets floor, including before the epoch.** A bucket is `floor(t / width) * width`, so `observed_at
  = -50` with width 100 lands in bucket `-100`, not `0` — which truncating division would get wrong.
* **Exceeding the group cap is an error, never a truncated answer.** `MAX_GROUP_COUNT` is 10,000; a
  caller silently handed the first 10,000 groups of a `group by subject` would have no way to know the
  answer was wrong.

Row-shaping fields (`limit`, `offset`, `order`, `newest_first`, `resolve_names`) describe how *rows*
come back and mean nothing for an aggregate, so a selection carrying them is rejected rather than
quietly ignored.

Groups come back ordered deterministically by key (kind first, then the kind's value), so repeated runs
and index-selected versus scanned evaluation agree on order as well as content.

## Planning and `explain_query` (Phase 16)

Which rows a query reads is a *cost* decision, never a semantic one: a source is only offered to the
planner when it provably contains every row the query could match, so the answer is the same whichever
is chosen. `explain_query` returns the plan `query` would follow without running it.

```
chosen: columnar_scan       estimated_rows: 250000    estimated_cost: 227500
considered:
  subject_index             rejected: query names no subject
  predicate_current_index   rows: 62500   cost: 321925
  columnar_scan             rows: 250000  cost: 227500
  row_scan                  rows: 250000  cost: 385000
```

* **Index row counts are exact**, not sampled: the planner reads a bucket's size or performs a binary
  search, which is why it needs no persisted cardinality statistics.
* **Costs are modelled from measured constants** (`docs/benchmarks.md`) and are comparable *only* against
  the other options in the same plan — they are nanosecond-shaped, not nanosecond-accurate, and are
  calibrated at one corpus size.
* **An index is not taken merely because it applies.** An index row costs several times a scanned column
  row, so a predicate matching most of the corpus is cheaper to scan. On a very small corpus, scanning
  beats any index, because a hash probe plus building a vector of ids costs more than streaming a few
  rows.
* **Rejections carry a reason**, and the ones worth reading are the correctness rejections: the
  current-state indexes hold only Active open-ended rows, so a query that is not current-shaped cannot
  use them at all.
* The plan `explain_query` returns is produced by the same call the executor makes, so it cannot describe
  something other than what will run.

## Paging with cursors (Phase 18)

Two ways to walk a result larger than one page:

```cpp
Query query;
query.order = QueryOrder::ObservedAt;
query.limit = 100;

std::string cursor;
while (true) {
    query.cursor = cursor;
    QueryResult page = kernel.query(query);
    // ... use page.assertions ...
    if (page.next_cursor.empty()) {
        break;              // that was the last page
    }
    cursor = page.next_cursor;
}
```

* **`next_cursor` is present only when a further page exists**, so a walk ends when it stops arriving
  rather than by counting rows. It is an opaque token: pass it back, do not parse it.
* **A cursor is exact.** Every ordering breaks ties on `AssertionId`, so `(ordering key, id)` is a total
  order over the result and "strictly after this position" names exactly one place in it. No row is
  returned twice and none is skipped.
* **A cursor survives a concurrent writer; an offset does not.** Offset is a count, so a row committed
  ahead of your position shifts every later page — walking `newest_first` while someone commits, page two
  by offset re-returns rows page one already gave you. A cursor names a position in the ordering, so it
  cannot. Against a read-only open (Phase 13) the question does not arise at all: that view is fixed at
  open, so the same walk returns the same rows however long it takes.
* **A cursor is cheaper at depth.** It does not skip the selection pass — every candidate row is still
  filtered on every page — but offset must order `offset + limit` rows to know which its page holds, and
  that prefix grows as you go deeper. Measured on 10,000 rows in pages of 100: 19.7 ms for the whole walk
  by offset against 4.0 ms by cursor, and 117 µs for the deepest offset page against 21 µs for the first
  (`docs/benchmarks.md`).
* **Rejected, not guessed at:** a cursor together with a non-zero `offset`, a cursor produced under a
  different `order`/`newest_first`, and a malformed or unknown-version token. Each would otherwise return
  a plausible-looking wrong window.
* An aggregate takes no cursor: it returns groups, not rows.

## Resource budgets (Phase 18)

| Budget | Field | Applies to |
| --- | --- | --- |
| Rows examined | `Query::max_rows_examined` | `query`, `aggregate`, `spill_query` |
| Group count | `AggregateQuery::max_groups` | `aggregate` |
| Result rows | `MAX_QUERY_RESULT` / `MAX_SPILL_ROWS` | always, not caller-settable |

Exceeding one of the first two throws `QueryBudgetExceeded`, which carries the budget's name, its limit,
and what was reached. It derives from `std::runtime_error`, so a caller that does not care can treat it
like any other failure; over MCP it comes back as a tool error naming the budget.

* **Rows examined, not elapsed time.** A wall-clock budget would make the same query against the same
  corpus succeed or fail depending on machine load. Everything else here is reproducible — the plan
  explains itself, the answer does not depend on how it was reached — and a timeout would be the one part
  that is not. `max_rows_examined` bounds work deterministically instead.
* **Opt-in.** `0` means no budget, which is what every query written before the field existed carries. The
  standing protection is the result ceiling, not a default budget.
* **What "examined" counts** is candidate rows, and how many that is depends on the plan: an index path
  charges per candidate id and stops early, while the columnar path's passes are whole-column and are
  refused up front. So a budget can decide *whether* a query throws on one path and not another — it still
  cannot change which rows a query returns.
* A group cap is never satisfied by truncation. A partial aggregate is a wrong answer that looks like a
  right one, so exceeding `max_groups` is an error.

## Discovery (Phase 18)

A `Query` is discoverable from its schema; what is *in* a given store is not. Two calls answer that.

`describe_predicates()` — every interned predicate, ascending by id, with its current row count
(`Active` and open-ended, i.e. what `current_by_predicate` would return):

```json
[{"id": 1, "name": "works_at", "current_rows": 412}, {"id": 2, "name": "lives_in", "current_rows": 88}]
```

Call it before naming a predicate. A query against a predicate that was never interned returns no rows,
which is indistinguishable from the fact being absent. A predicate interned but never asserted still
appears, with `current_rows: 0`.

`describe_corpus()` — the shape of the store: `assertion_count`, `entity_count`, `predicate_count`,
`distinct_subjects`, `distinct_current_objects`, a count per status (every status, zeros included), and
the `observed_at` / `valid_from` spans. Enough to decide whether to page or spill, and which time windows
contain anything.

* `valid_to` is deliberately **not** summarized: `OPEN_ENDED` is `0`, so its minimum would report 1970 for
  any store holding a single current fact.
* The spans are absent (`null`) on an empty corpus rather than `0`.
* Both are snapshots of the moment they were called, and both work on a read-only open. `describe_corpus`
  is one linear pass (~18 µs per 10,000 assertions), so it is for shaping a query, not for calling inside
  a loop.

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
