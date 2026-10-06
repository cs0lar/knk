# Storage Format

This document describes the on-disk binary format shared by all seven persisted logs: the assertion
log (`AssertionLog`, the `segments/` directory — see "Segmented assertion log" below),
`indexes/subject.idx` (`SubjectIndexLog`), `indexes/current.idx` (`CurrentIndexLog`),
`indexes/observed_time.idx` (`ObservedTimeIndexLog`), `catalog/entities.log`/
`catalog/predicates.log` (`EntityCatalogLog`/`PredicateCatalogLog` — see "Entity/predicate catalog"
below), and `provenance/provenance.log` (`ProvenanceLog` — see "Provenance log" below).

All seven logs use the identical per-file header + record-frame layout described below; what differs
per log is the record payload — fixed-size raw structs for the first four, explicitly serialized
variable-length payloads for the two catalog logs and the provenance log (see "Entity/predicate
catalog" and "Provenance log") — and only the assertion log spreads that framing across multiple
files instead of one; every other log remains a single file.

## Storage root lock

`LOCK`, at the storage root alongside `assertions.log`, is not a log or a data file: it exists
purely as the target of an OS-level advisory lock (POSIX `flock()`, `LOCK_EX | LOCK_NB`), taken by
`StorageEngine`'s constructor and held for its entire lifetime (`kernel/storage_lock.hpp`). Its
contents are irrelevant and never read — only the lock state on the open file description matters.

**Invariant enforced:** at most one live `StorageEngine`/`KnowledgeKernel` may hold a given storage
root open at a time, matching the "single writer" model `AGENTS.md`'s Concurrency Rules section
declares. **Why:** nothing previously checked this — two processes (or two live objects in one
process) opening the same root would both replay the logs and both start appending, silently, with
no error at either point; the failure only surfaced later as a log that no longer replayed cleanly.
**Failure mode:** a second open while the root is held throws `std::runtime_error` naming the path;
the holder is completely unaffected. **How it's tested:** `tests/storage_lock_tests.cpp` covers
acquire/conflict/release directly, including a fork+`SIGKILL` test proving the lock is released by
the kernel on an ungraceful holder exit with no manual cleanup; `tests/knowledge_kernel_tests.cpp`
covers the same behavior through the public `KnowledgeKernel` API.

**Read-only opens take no lock at all (Phase 13).** `OpenMode::ReadOnly` opens a root for querying
alongside a live writer, and it neither creates nor locks anything. The obvious alternative — readers
take `flock(LOCK_SH)` on this same file — cannot work: the writer holds `LOCK_EX` on it for its whole
lifetime, and `LOCK_EX` excludes `LOCK_SH`, so a shared-lock reader could never open alongside the
writer it exists to coexist with. `LOCK` therefore stays purely the single-writer guard, and a reader
(which writes nothing, so can corrupt nothing) does not participate in it. If a future destructive
operation — a compaction, say — needs to wait for readers to drain, that wants its own reader-presence
lock, added then rather than pretended now.

What a read-only open guarantees, and what it does not:

* **It creates nothing.** Not the root, not a subdirectory, not `LOCK`. A read-only open of a
  non-existent root throws rather than bringing one into being.
* **It writes nothing.** Every mutating method on `StorageEngine` and `KnowledgeKernel` throws, guarded
  at both layers, and `tests/read_only_open_tests.cpp` fingerprints every byte under the root before and
  after a reader opens, queries and aggregates, requiring them identical.
* **It cannot repair derived state.** A stale checkpoint or a corrupt index log is rebuilt *in memory*
  and answered correctly from the log — which is the source of truth — but the files are left exactly as
  found. The consequence worth knowing operationally: such a root makes every read-only open pay a full
  replay, until a writer opens it once and heals it.
* **It sees a committed prefix, not a torn record.** Tail-tolerant reads drop a half-written trailing
  frame exactly as crash recovery does. A batch commit in flight may be *partially* visible, for the same
  reason a crash can leave a prefix of one durable (see "Durability" above) — a reader is not a
  transaction boundary.
* **It is a snapshot as of its own construction,** not a live view: replay happens once, in the
  constructor, so commits made afterwards need a reopen to be seen.

This file is not covered by the shared header/record-frame format below — it holds no records, so
there is no format to version.

## File layout

Every individual file involved (each assertion-log segment file, each of the three index log files,
the two catalog log files, and the provenance log file) follows this same layout:

```
[ file header, 8 bytes ]
[ record frame ]
[ record frame ]
...
```

A file that does not exist, or exists with zero bytes, is treated as an empty log (no records) —
this is the state before the first `append()`/`overwrite_all()` call.

### File header (8 bytes)

| Field   | Size (bytes) | Value                                 |
|---------|--------------|----------------------------------------|
| magic   | 4            | ASCII `"KNK1"`                          |
| version | 4            | `uint32_t`, little-endian on this host; currently `1` |

The header is written once, the first time a log file is created (on the first `append()` against
a missing/empty file, and at the start of every `overwrite_all()`).

A file with fewer than 8 bytes (a torn write mid-header, from a crash on the very first-ever
append to a brand-new file) is treated the same as a missing/empty file — by construction no
record frame can exist without a complete header preceding it, so an incomplete header
unambiguously means zero records were ever durably completed. A full 8-byte header with the wrong
magic or an unrecognized version is still treated as corrupt and `read_all()` throws
`std::runtime_error` — a torn write cannot produce a full-length-but-wrong header, so this is a
genuine format mismatch (e.g. a stale pre-checksum file), not a crash artifact.

(For the assertion log specifically, "torn header" tolerance only applies to the active/last
segment file — see "Segmented assertion log" below for why a torn header in an earlier segment is
instead unambiguous corruption.)

There is no migration path from pre-checksum log files (files without this header). This is a
breaking on-disk format change — existing local data directories must be deleted and rebuilt from
scratch (the log is always rebuildable by replaying `assertions.log`, or from scratch if
`assertions.log` itself predates this format).

### Record frame

```
[ uint32_t record_size ]
[ raw struct bytes, record_size bytes ]
[ uint32_t crc32 ]
```

- For the four fixed-size record types, `record_size` is redundant with the record's compile-time
  `sizeof(...)` and exists purely as an early sanity check; a mismatch throws
  `std::runtime_error`. The two catalog logs have variable-length payloads instead (see
  "Entity/predicate catalog"), so `record_size` there is load-bearing, not redundant — but the same
  "any framing inconsistency always throws, never tail-tolerant" rule still applies to it.
- `crc32` is the CRC-32 (IEEE 802.3 polynomial, `0xEDB88320`, same table-based algorithm as zlib)
  of the payload bytes only — it does not cover `record_size`. See `include/kernel/checksum.hpp`.
- On read, if a frame is truncated anywhere (short `record_size`, short payload, or short `crc32`),
  the record is silently dropped and reading stops — this is treated as a crash-torn trailing
  write, not corruption.
- A `record_size` mismatch always throws `std::runtime_error`: a bad size breaks framing entirely,
  so there is no safe way to locate where the next record would begin.
- A checksum mismatch on a *fully present* frame is tail-tolerant: if nothing follows that frame in
  the file, it is silently dropped (same treatment as a truncated frame — a torn write can only
  ever leave garbage at the true end of the file, so a checksum failure with nothing after it is
  indistinguishable from an ordinary crash mid-append). If valid-length data *does* follow the bad
  frame, `read_all()` throws `std::runtime_error` instead — data cannot validly follow a torn
  write, so this is unambiguous real corruption (bit rot, tampering, disk error), not a crash
  artifact. This is a deliberate policy choice: it accepts that a corrupted-but-truly-last record
  is indistinguishable from a torn write (the same ambiguity already inherent in tolerating short
  trailing frames), in exchange for automatic recovery from ordinary crashes.
  (For the assertion log, "nothing follows" means nothing follows anywhere in the log, not just
  within one segment file — see "Segmented assertion log" below.)

### Record payloads

- `AssertionLog`: raw `knk::Assertion` (`include/kernel/assertion.hpp`) — `id`, `subject`,
  `predicate`, `object`, `valid_from`, `valid_to`, `observed_at`, `confidence`, `status`,
  `supersedes_id`, `retracts_id`.
- `SubjectIndexLog`: `SubjectIndexRecord` — `{subject, assertion_id}`.
- `CurrentIndexLog`: `CurrentIndexRecord` — `{subject, predicate, assertion_id, active}`.
- `ObservedTimeIndexLog`: `ObservedTimeIndexRecord` — `{subject, observed_at, assertion_id}`.
- `EntityCatalogLog`/`PredicateCatalogLog`: variable-length, explicitly serialized — see
  "Entity/predicate catalog" below.

## Entity/predicate catalog

`catalog/entities.log` (`EntityCatalogLog`) and `catalog/predicates.log` (`PredicateCatalogLog`)
persist the `EntityId`/`PredicateId` <-> name/value mappings used by `KnowledgeKernel::intern_entity`/
`intern_value`/`intern_predicate`. They reuse the exact same 8-byte header and
`[record_size][payload][crc32]` framing as the other four logs, but are the **first variable-length**
record payloads in the codebase — every other log's payload is a fixed-size raw struct where
`record_size == sizeof(Record)`; these two serialize fields explicitly instead.

`EntityCatalogRecord` payload bytes: `[EntityId id (8)][uint8_t kind (1)][kind-specific payload]`,
where `kind` is `ValueKind` (`include/kernel/value.hpp`) and the payload is:

- `Text`: `[uint32_t length][length bytes]`
- `Int64` / `Timestamp`: `[int64_t]` (8 bytes)
- `Double`: `[double]` (8 bytes)
- `Bool`: `[uint8_t]` (1 byte)

`PredicateCatalogRecord` payload bytes are always text: `[PredicateId id (8)][uint32_t
length][length bytes]`.

Because the payload is self-describing rather than a fixed size, there is one more way for a frame
to be malformed beyond the shared rules above: a `record_size` smaller than the minimum possible
payload (9 bytes for entities, 12 for predicates), or a length prefix that does not exactly consume
the remaining payload bytes. Both always throw `std::runtime_error`, no tail tolerance — the same
treatment as a `record_size` mismatch in the fixed-size logs, since there is no safe resync point
once framing or internal structure is inconsistent.

**These two logs are authoritative, not derived/rebuildable indexes.** Unlike the three Phase 3
index logs, nothing in `assertions.log` records what an `EntityId`/`PredicateId` means — assertions
only ever store the ids themselves — so there is no way to reconstruct a lost or corrupt catalog
mapping by replaying the assertion log. Consequently:

- Neither log has an `overwrite_all` method; there is no self-heal path.
- Non-tail corruption is fatal, uncaught `std::runtime_error` propagating straight out of
  `KnowledgeKernel`'s constructor — the same treatment `AssertionLog` gets, not the Phase 3 indexes'
  catch-and-rebuild behavior (see "Recovery behavior" below).
- Tail-tolerant behavior (a torn header, an incomplete trailing frame, or a checksum mismatch with
  nothing following it) is unchanged from the other logs — that's about surviving an ordinary crash
  mid-append, which is orthogonal to whether the log is derived or authoritative.

`EntityId`s minted by `EntityCatalogLog` share the same id space as `EntityId`s a caller assigns
directly without ever interning them (e.g. `examples/knowledge_kernel_demo.cpp`'s
`EntityId external_feed = 9000;`). Avoiding
collisions between manually chosen ids and catalog-interned ids is the caller's responsibility, same
as it already is for all `EntityId` usage today; this phase adds an optional sub-allocator for the
subset of entities a caller chooses to intern by name/value, not a new authority over the whole
`EntityId` space.

## Payload store

`payloads/` (`PayloadStore`) holds arbitrary-size byte content addressed by `EntityId`, for content
too large or unstructured to fit the catalog's small fixed-ish framed-log records (whole documents,
arbitrary blobs). Unlike every log described above, it is **not** a single file: it is one file per
payload, `payloads/<id>.payload`, with no manifest — `existing_ids()` discovers what's on disk purely
by scanning the directory's filenames, the same "derive everything from directory contents" precedent
used by the segmented assertion log below.

Each payload file is always written in full via `write_file_atomically` (temp file, fsync, atomic
rename, fsync parent directory), never appended to — the same mechanism `indexes/checkpoint` and
`snapshot` use. This matters for its corruption model: because a crash mid-write can never leave a
half-written file visible at the real path, a file that exists at `payloads/<id>.payload` is
guaranteed to be either fully-formed or entirely absent. There is therefore no "torn write" category
to tail-tolerate here, unlike the append-only logs — any anomaly found in a present file is genuine
corruption, and `read()` always throws rather than degrading to "no usable data."

Format: `[4-byte magic "KNKD"][4-byte uint32 version][8-byte uint64 content length][length bytes of
content][4-byte uint32 crc32]`, where the crc32 covers the content bytes only. The length is
validated against the actual file size before the content buffer is allocated, so a corrupted length
field can never drive a huge allocation — the same defensive check `snapshot`'s `record_count` uses.

**`PayloadStore` is authoritative, not a derived/rebuildable index**, for the same reason the entity/
predicate catalog logs are: `assertions.log` never stores payload content, so there is nothing to
rebuild a lost or corrupt payload from. Consequently there is no `overwrite_all`/self-heal path, and
non-tail corruption is a fatal, uncaught `std::runtime_error` — `KnowledgeKernel`'s constructor reads
(not just lists) every payload found on disk at startup via `existing_ids()`, so a corrupt payload
file is detected and thrown eagerly at startup, not lazily on first access.

`EntityId`s minted for documents (`KnowledgeKernel::intern_document`, via `Catalog::allocate_entity_id`)
share the same id counter/space as `EntityId`s minted by `intern_entity`/`intern_value`, but — unlike
those — have no corresponding record in `catalog/entities.log`: a document has no name or scalar value
to intern, only content, so there is nothing meaningful to write there. This means the counter's
continuity across restarts can't rely on replaying `entities.log` alone; `KnowledgeKernel`'s
constructor also feeds every id discovered via `PayloadStore::existing_ids()` back into
`Catalog::note_allocated_entity_id`, which advances the same counter `add_entity` does but without
recording any name/value mapping. Skipping this step would let a restart mint a document id that
collides with a document id issued before the restart.

## Provenance log

`provenance/provenance.log` (`ProvenanceLog`) records *which source produced a given assertion, and
by what method* — the durable backing for `KnowledgeKernel::record_provenance`/`provenance_for`. It
is a **side-log keyed by `AssertionId`**, deliberately not a new field on `Assertion`: adding a field
would be a breaking change to the raw-struct on-disk assertion format (see "Known limitation" below),
whereas a side-log leaves `assertions.log` untouched.

It reuses the exact same 8-byte header and `[record_size][payload][crc32]` framing as the other logs,
with a variable-length payload — the same precedent as the catalog logs' `Text` case.

`ProvenanceRecord` payload bytes: `[AssertionId assertion_id (8)][EntityId source (8)][Timestamp
recorded_at (8)][uint32_t method_length (4)][method_length bytes]`. The minimum payload is 28 bytes
(an empty `method` string). As with the catalog logs, a `record_size` below that minimum, or a
`method_length` prefix that does not exactly consume the remaining payload bytes, always throws
`std::runtime_error` with no tail tolerance — there is no safe resync point once framing is
inconsistent.

`source` is just an `EntityId`, interned into `Catalog` exactly like any other entity (a person, an
ingestion pipeline, an agent), so resolving "which source produced this claim" reuses
`entity_name`/`entity_value` for free rather than introducing a second identity system. `recorded_at`
is a caller-supplied `Timestamp`, matching how `observed_at` is supplied to `commit` — the kernel
never reads a wall clock, so provenance replay stays deterministic.

The log is append-only, so more than one record may exist for the same `AssertionId` (provenance can
be re-recorded); on replay `KnowledgeKernel` keeps the **last** record read for each id, so the newest
recorded provenance wins. `record_provenance` validates that its target `AssertionId` refers to an
existing assertion before appending, so a failed call never persists a dangling provenance record —
the same pre-append validation `commit_retraction`/`commit_superseding` apply to their targets.

**`ProvenanceLog` is authoritative, not a derived/rebuildable index**, for the same reason the catalog
logs and payload store are: `assertions.log` encodes nothing about provenance, so there is nothing to
rebuild it from. Consequently there is no `overwrite_all`/self-heal path, and non-tail corruption is a
fatal, uncaught `std::runtime_error` propagating straight out of `KnowledgeKernel`'s constructor —
replayed in the same uncaught block as the catalog logs and payload store, before the
snapshot/checkpoint/tail-vs-full-replay branching that only concerns the derived Phase 3 indexes.

## Entity merge log

`catalog/entity_merges.log` (`EntityMergeLog`) records one-way entity-merge redirects — the durable
backing for `KnowledgeKernel::merge_entities`/`resolve_entity` and `Catalog::resolve`. It reuses the
exact same 8-byte header and `[record_size][payload][crc32]` framing as the other logs, but with a
**fixed-size** payload (like `ObservedTimeIndexLog`, not the variable-length catalog/provenance logs),
since every field is a fixed-width integer: `EntityMergeRecord` is `[EntityId absorbed (8)][EntityId
surviving (8)][Timestamp merged_at (8)]`, 24 bytes total. A `record_size` other than exactly 24 always
throws `std::runtime_error`, with the same tail-tolerant treatment of a torn trailing frame as every
other log (indistinguishable from a crash mid-append).

`merged_at` is a caller-supplied `Timestamp`, matching how `observed_at`/`recorded_at` are supplied
elsewhere — the kernel never reads a wall clock, keeping replay deterministic.

The log is append-only and one-way: merging keeps growing a chain of redirects (`A -> B`, then later
`B -> C`) rather than ever rewriting an earlier record. `Catalog::resolve` follows the chain
transitively at read time, so nothing here needs to be collapsed or rewritten when a later merge
extends the chain. `Catalog` itself only stores the direct, one-hop redirects (`merge_redirects_`);
transitivity is entirely a property of how `resolve` walks the map.

**`EntityMergeLog` is authoritative, not a derived/rebuildable index**, for the same reason the catalog
and provenance logs are: `assertions.log` never records that two `EntityId`s were merged, so there is
nothing to rebuild this mapping from. Consequently there is no `overwrite_all`/self-heal path, and
non-tail corruption is a fatal, uncaught `std::runtime_error` propagating straight out of
`KnowledgeKernel`'s constructor — replayed in the same uncaught block as the catalog logs, payload
store, and provenance log, before the snapshot/checkpoint/tail-vs-full-replay branching that only
concerns the derived Phase 3 indexes.

**Assertions are never rewritten by a merge.** `assertions_` and every persisted index keep whatever
`EntityId` was originally committed as a subject/object; resolution happens only at the query
boundary. Every `KnowledgeKernel` query method that takes a caller-supplied subject `EntityId`
(`assertions_for_subject`, `current`, `hypotheses_for`, `neighbors`, `co_occurring_predicates`,
`valid_at`, `known_at`, `valid_at_known_at`, `valid_time_timeline`, `observed_time_timeline`,
`commit_history`, `find_conflicts`) calls `Catalog::resolve` on that subject before doing anything
else, so a caller still holding an absorbed id transparently gets the surviving id's results. Catalog
name/value lookups (`entity_name`, `entity_value`, `predicate_name`, `find_entity`, `find_value`,
`find_predicate`, `document_content`) are deliberately **not** resolved — they answer "what is this id
called/worth," a fact about the specific id, not "which real-world entity does this id represent,"
so merging does not change what they return for the absorbed id.

## Segmented assertion log

Unlike the three index logs, the assertion log (`AssertionLog`) is not a single file. It is a
directory of fixed-capacity segment files:

```
segments/
  0000000000.seg   (ids 1..max_records_per_segment)
  0000000001.seg   (ids max_records_per_segment+1..2*max_records_per_segment)
  ...
```

`StorageConfig::max_records_per_segment` (default 100,000 — a storage-layout placeholder, not a
tuned performance number, since nothing above `AssertionLog` can observe segment boundaries) is
fixed per data directory: segment index `k` deterministically holds ids
`[k*max_records_per_segment + 1, (k+1)*max_records_per_segment]`. Each segment file uses the exact
same header + record-frame format described above; only the layout of *which file* a given id lives
in is new.

**Why this arithmetic is exact, not a hint.** `append()` checks capacity *before* writing a record,
so a segment is only ever rolled from after its previous record was already fully written and
fsynced in an earlier, separate call. This guarantees every non-active (already-rolled-from) segment
holds *exactly* `max_records_per_segment` complete records — never fewer, never torn. Only the
single currently-active (highest-index) segment can ever be short (still filling) or have a torn
trailing frame from a crash. This is what lets `read_after()` and `record_count_hint()` skip or size
whole historical segments via pure index arithmetic, with no manifest or segment-metadata file:

- `read_after(last_seen_id)` skips any segment whose entire id range is `<= last_seen_id` without
  opening it, seeks within the (at most one) segment straddling `last_seen_id` using the same
  byte-offset trick as a single-file log (just relative to that segment's own starting id), and
  reads every later segment in full.
- `record_count_hint()` is `highest_segment_index * max_records_per_segment` (exact, for every
  earlier segment) plus a file-size-based estimate for just the active segment (same technique as a
  single-file log, scoped to one file).

**Corruption tolerance generalizes to "only the active segment is tail-tolerant."** A torn header,
an incomplete trailing record, or a checksum mismatch with nothing following it is silently dropped
only in the active (last) segment — exactly as it would be in a single-file log. The same anomaly in
an earlier, already-rolled-from segment always throws instead: since a non-active segment is
guaranteed complete by construction (see above), a torn-looking trailing frame there cannot be an
ordinary crash artifact — it's unambiguous corruption. Concretely, this means `AssertionLog`'s shared
read loop takes a `tolerate_trailing_anomaly` flag that's true only when reading the segment that is
currently the highest-indexed one on disk.

**This is a breaking, non-migrated on-disk format change**, same precedent as the checksum format
change earlier in this document: there is no migration from a pre-segment single `assertions.log`
file. Existing local data directories must be deleted and rebuilt from scratch.

### Segment archival

`AssertionLog::archive_segments_before(AssertionId)` (exposed as `StorageEngine::archive_segments_before`
and `KnowledgeKernel::archive_segments_before`) is **compaction, not deletion** — the Phase 8 answer to
"pruning" in the roadmap. It moves already-rolled-from segment files from `segments/` into
`segments/archive/`, unchanged byte-for-byte; nothing is rewritten or shrunk.

A segment is eligible only if both hold:

- **Not the active segment.** The active (highest-index) segment may still receive writes and is
  never guaranteed complete, so it is skipped unconditionally regardless of the requested threshold —
  the same "only non-active segments are guaranteed exactly `max_records_per_segment` complete
  records" invariant the rest of this section relies on.
- **Entirely before the threshold.** A segment's *end* id (`(index + 1) * max_records_per_segment`,
  exact for any non-active segment) must be strictly less than the requested `AssertionId` — i.e.
  every id the segment holds is `< assertion_id`. A segment straddling the threshold is left alone.

Archiving is idempotent: a segment already moved simply no longer appears in `segments/` on a repeat
call, so nothing happens to it a second time.

**Archived segments remain fully readable.** `read_all()`/`read_after()`/`record_count_hint()` resolve
a segment's location by checking `segments/<index>.seg` first and falling back to
`segments/archive/<index>.seg` — a segment lives in exactly one of the two locations at a time, never
both — so callers see identical results whether or not `archive_segments_before` has ever run. This is
the concrete guarantee behind "pruning never shrinks queryable history": audit and timeline queries,
which read the full log, are completely unaffected by archival.

True, irreversible deletion is an explicit **non-goal**, not deferred future work — see `AGENTS.md`'s
Phase 8 section.

## Columnar projection (`columns/`)

A column-oriented projection of the assertion log: ten fixed-width arrays, one per `Assertion` field, in
assertion-id order — row *i* is id *i+1*. Added in Phase 14 as the substrate Phase 15's vectorized
execution needs; a filter on one field touches one contiguous byte range instead of striding over
88-byte records.

```text
columns/
├── manifest              row count + one checksum per column
├── subject.col           uint64 per row
├── predicate.col         uint64
├── object.col            uint64
├── valid_from.col        int64
├── valid_to.col          int64
├── observed_at.col       int64
├── confidence.col        double (IEEE 754, native byte order)
├── status.col            uint8  (AssertionStatus's underlying value)
├── supersedes_id.col     uint64
└── retracts_id.col       uint64
```

Each column file is `[4-byte magic "KNKL"][uint32 version][uint64 element size]` followed by raw
elements. The element size is stored so a stride change is caught as a format mismatch rather than read
as garbage. `status.col` makes the numeric values of `AssertionStatus` part of the on-disk format, which
is why `status.hpp` now spells them out explicitly.

**The projection is verbatim, not effective.** Each row is the log record *as appended*, including the
status it was appended with. A superseded row's `status.col` byte still says `Active`, because
append-only storage never rewrote it — the `Superseded` status exists only in replayed in-memory state.
That is why `supersedes_id`/`retracts_id` are part of the projection: together with `status` they make
effective status derivable from the columns alone (a row is superseded or retracted exactly when some
later row points at it), which is the same derivation replay performs and the one Phase 19 will restrict
by id. Mirroring effective status instead would mean rewriting an arbitrary earlier row on every
supersession — an in-place write into a fixed-stride file, plus a full checksum recomputation.

**Integrity works differently here than anywhere else in the storage root.** Every log carries a CRC per
record; fixed-stride columns cannot, without giving up the stride that makes them worth having. Instead
`columns/manifest` holds the row count and one CRC-32 per column, written with the same atomic
temp-then-rename as the checkpoint, and self-checksummed so a truncated manifest is rejected rather than
read as a row count. Appends continue each column's checksum incrementally (see `crc32_update` in
`checksum.hpp`) — recomputing over every row per append would make appending O(store) rather than
O(appended), i.e. quadratic over the store's life.

**Recovery is the Phase 3 index treatment, not the log's:** never fatal, always rebuildable.
`StorageEngine`'s constructor verifies the store and then either

* **rebuilds** it wholesale from `assertions.log` — a store that fails verification, or a root written
  before this phase existed and has no `columns/` at all (a one-time O(log) migration); or
* **appends the missing tail** — a store that merely lags, which is the ordinary crash-between-appends
  case, since `append_assertion` writes the log first and the columns second on purpose.

Columns are maintained inside `StorageEngine::append_assertion`/`append_assertions`, so every commit path
— single, superseding, retraction, hypothesis, and batch — stays in lockstep without having to remember.

**A read-only open neither builds nor repairs them** (both are writes). It serves queries from the log,
which is always authoritative, and leaves a missing or damaged `columns/` exactly as found.

Like the raw-struct serialization noted below, these files are native-endian and native-layout: portable
only between builds that agree on those.

## Spilled query results (Phase 17)

A result too large to travel as JSON-RPC text is written to disk as columns and handed over as a
descriptor. These files are **not** part of a storage root: they live in a directory the caller owns,
which is what lets a read-only kernel — the analytics case — produce them while writing nothing to the
store it opened.

```text
<spill-dir>/<token>/
├── descriptor.json       format, version, row count, byte order, one entry per column
├── dictionary.json       the ids present, mapped to catalog names/values
├── id.col                uint64 per row
├── subject.col           uint64
├── predicate.col         uint64
├── object.col            uint64
├── valid_from.col        int64
├── valid_to.col          int64
├── observed_at.col       int64
├── confidence.col        double
├── status.col            uint8   (the *effective* status; see below)
├── supersedes_id.col     uint64
└── retracts_id.col       uint64
```

Column files have **no headers** — they are nothing but fixed-width native-endian values, because
`descriptor.json` already says how many rows there are and how wide each value is. A reader is about
twenty lines in any language; `tools/read_spill.py` is the reference one, and converting to Arrow is a
few lines on top of it.

**Why not Arrow IPC.** The plan in #57 recommended hand-writing Arrow IPC: interoperability without a
dependency. Implementing it surfaced what that recommendation had glossed over — Arrow IPC's metadata is a
**FlatBuffer**, not a header, so "write the spec by hand" means implementing FlatBuffer encoding (vtables,
offsets, alignment) with no Arrow implementation available to check the result against. Neither the
kernel's development environment nor its CI (ubuntu-latest, C++ only) can install pyarrow, so the only
test possible would be a hand-written reader agreeing with the hand-written writer — which proves nothing
about whether DuckDB or Polars can read the file. An interoperability claim that cannot be tested is one a
*user* discovers is false. Vendoring Arrow C++ stays rejected on size. The trade accepted instead: a
consumer writes a few lines of conversion, on their side, where a real Arrow implementation exists.

Three details worth knowing:

* **The status column holds effective status**, unlike `columns/` in the storage root, which is a verbatim
  projection of the log and records the status each row was *appended* with. A spill is a query result, so
  it must say what a query says: a superseded row reads `Superseded` here and `Active` there.
* **The dictionary is O(distinct ids), not O(rows).** A column of ids is useless to a consumer without the
  catalog, and exporting it per row would be enormous; exporting only the ids the result mentions is
  small, and lets the consumer join locally.
* **Lifecycle is the caller's.** Nothing sweeps spills automatically: the kernel never reads a wall clock,
  so a TTL is not available to it, and deleting a result someone is still reading would be worse than
  leaving a file behind. `drop_spill` removes one; an existing token is refused rather than overwritten.

Row count is bounded by `MAX_SPILL_ROWS` (10,000,000) — generously, since escaping the 10,000-row JSON
ceiling is the point, but bounded all the same because an accidentally unbounded spill filling a disk is a
real failure mode.

## Durability

Every `append()` closes its `std::ofstream` and then fsyncs the file (`knk::fsync_file`,
`include/kernel/durability.hpp`) before returning, so a commit is not considered durable until the
data has actually reached physical disk, not just the OS page cache.

`append_batch()` (the batch counterpart on `AssertionLog`, the three index logs, and the provenance
log — used by `KnowledgeKernel::commit_batch` and `record_provenance_batch`) writes every record of a batch under **one** open/close/fsync per
file instead of one per record — the record bytes on disk are byte-identical to what the same
records appended one at a time would produce, so nothing downstream can tell the two apart. What
changes is the size of the crash window, not the format:

* A crash before the fsync returns can leave any **prefix** of the batch durable — never a gap and
  never a reordering, since records are written in order and only the trailing frame of the active
  segment can be torn (which `read_all`/`read_after` already drop). Batch ids are consecutive, so the
  surviving prefix is a contiguous id range and the caller resumes at the first missing one.
* This is deliberately **not** an atomic multi-record write. Making it one would need a
  batch-commit marker record, i.e. a new record type and a format version bump for every reader;
  `commit_batch`'s contract promises the prefix guarantee the current format actually supports
  rather than an atomicity it doesn't.
* `AssertionLog::append_batch` still fills a segment to capacity and fsyncs it before opening the
  next one, so the "every non-active segment holds exactly `max_records_per_segment` complete
  records" invariant that makes segment boundaries pure id arithmetic holds mid-batch too.
* An empty batch writes nothing at all — no header-only file or segment is created.

`overwrite_all()` (the three index logs' self-heal rewrite) writes via `knk::write_file_atomically`:
the full new content is written to a `.tmp` file, fsynced, then `rename`d over the real path
(atomic on the same filesystem), followed by an fsync of the parent directory (the standard
"durable rename" pattern, so a crash right after the rename can't lose the directory entry). A
crash mid-rewrite therefore always leaves either the complete old file or the complete new file,
never a torn one.

Both are POSIX-only (`open`/`fsync`/`close`), with no cross-platform abstraction — an accepted
limitation for early local development, same treatment as the raw-struct-serialization limitation
below.

## Index checkpoint

`indexes/checkpoint` is a fifth, distinct file: a single atomically-replaced value (not an
append-only log), tracking the highest `AssertionId` whose index-log writes are known to have
fully completed. It exists to close a gap that checksums alone don't: `commit()` /
`commit_superseding()` / `commit_retraction()` each append to `assertions.log` first, then to the
three index logs — a crash between those two steps leaves `assertions.log` with a record that
none of the index logs know about, and critically, none of the index logs' `read_all()` calls
throw in that case (the newest entry is simply *missing*, indistinguishable from it never having
existed).

Format: `[4-byte magic "KNKC"][4-byte uint32 version][8-byte uint64 AssertionId][4-byte uint32
crc32]`. Unlike the four data logs, reading a checkpoint **never throws** — it is purely an
optimization hint for `KnowledgeKernel`'s startup fast path, not authoritative data. Any problem
(missing file, bad magic/version, bad crc) degrades to `0` (the codebase's existing "no assertion"
sentinel), which just costs an extra full replay on the next startup, never data loss.

`commit_batch` writes the checkpoint once, after the whole batch's index appends, so a crash
anywhere inside a batch leaves the checkpoint behind the log and the next startup rebuilds every
index from it — the same self-heal a torn single commit already gets, just covering more records.

`KnowledgeKernel`'s constructor trusts the persisted indexes only if every index file loaded
cleanly *and* the checkpoint equals the highest `AssertionId` present in `assertions.log`;
otherwise it falls back to the existing full-replay-and-self-heal path (see Recovery behavior
below), which also rewrites the checkpoint once the rebuild completes. Each commit method writes
the checkpoint as the last step of its storage-append sequence, after all index appends succeed
and before applying the change to in-memory state — so a crash at any point during a commit's
storage writes leaves the checkpoint reflecting only the last commit that fully completed.

## Snapshot

`snapshot` (at the storage root, alongside `assertions.log`, not inside `indexes/`) is a sixth,
distinct file: a full-replace snapshot of `assertions_`, written only when the application explicitly
calls `KnowledgeKernel::write_snapshot()` — there is no automatic cadence. Unlike the four append-only
logs, it is always fully rewritten (never appended to), same as `indexes/checkpoint`.

Note what this is *not*: `assertions_` (and therefore `assertions.log`) never shrinks — nothing is ever
compacted or truncated, because audit/timeline queries (`commit_history`, `valid_time_timeline`,
`observed_time_timeline`) need the full history forever. A snapshot does not reduce total data volume,
and taking one never allows `assertions.log` to be truncated or archived. Its value is narrower: it
turns "re-parse every individual framed record in `assertions.log` on every startup" into "one bulk
snapshot load, plus only the small tail committed since the snapshot was taken."

Format: `[4-byte magic "KNKS"][4-byte uint32 version][8-byte uint64 last_snapshotted_id][8-byte uint64
record_count][record_count * sizeof(Assertion) raw bytes][4-byte uint32 crc32]`, where the crc32 covers
`last_snapshotted_id`, `record_count`, and the assertion bytes together as one buffer (a single CRC over
the whole payload, not per-record CRCs like the four framed logs — safe here because
`write_file_atomically` already guarantees no torn file, so there's no tail-tolerance to preserve).

**Version 2 (Phase 19) stores each record's *appended* status, not its effective one.** The status byte
in the stored records is what the record was committed with — `Active`, `Hypothesis` or `Retraction` —
matching what `assertions.log` holds, rather than what the row's status has since become. Version 1
stored the effective status, which is lossy: a superseded row reads `Superseded` whether it was committed
`Active` or as a `Hypothesis`, so a snapshot of it could not say what was believed before the
supersession, and the snapshot fast path would answer an as-of query differently from a full replay. The
kernel re-derives effective status after loading, from the `supersedes_id`/`retracts_id` links the
records already carry — the same derivation replay performs.

Reading a v1 snapshot from a v2 build yields "no usable snapshot" by the version check below, which
costs one slow startup and loses nothing: the log is the source of truth and the snapshot is only ever a
hint. Nothing else in the store is affected, and a v2 build rewrites the file on the next
`write_snapshot()`.

Like `indexes/checkpoint`, reading a snapshot **never throws** — any anomaly (missing file, bad
magic/version, `record_count != last_snapshotted_id`, a file size that doesn't match the expected size
computed from `record_count`, or a bad crc) degrades to "no usable snapshot," never an error. The
expected-size check runs before any allocation sized by `record_count`, so a corrupted, implausibly
large `record_count` can never trigger a huge allocation attempt.

`AssertionLog` gained two supporting methods for this: `read_after(AssertionId last_seen_id)` seeks
directly to the deterministic byte offset for a given id (frames are fixed-size and ids are assigned
1..N with no gaps, an invariant already relied on elsewhere via `assertions_[id - 1]`) instead of parsing
from the start, and `record_count_hint()` is an O(1) file-size-based estimate (no parsing) used to
sanity-check that a snapshot doesn't claim to cover more records than the log could possibly contain.

`KnowledgeKernel`'s constructor only tries to use a snapshot on the same fast path already used by the
index checkpoint (indexes loaded cleanly *and* checkpoint matches): it seeds `assertions_`/`next_id_`
from the snapshot and then only needs to walk the log tail via `read_after`. The full-rebuild fallback
path (index files missing/corrupt, or checkpoint mismatch) ignores the snapshot entirely and re-reads
the whole log, because that path must `apply()` every record from id 1 to rebuild `IndexManager` from
scratch regardless — a snapshot provides no benefit there.

## Known limitation: raw struct serialization

Record payloads are still written via `reinterpret_cast`-style raw struct serialization, not
explicit field-by-field encoding. This means the on-disk format is sensitive to compiler ABI,
struct padding, enum underlying size, and host endianness — a log written on one platform/compiler
is not guaranteed to be portable to another. This is an accepted limitation for early local
development (per `AGENTS.md`'s Binary Format Rules) and is unchanged by the introduction of
checksums; replacing it with explicit serialization is a larger, separate Phase 4 concern.

## Recovery behavior

A non-tail checksum/header/size failure throws the same `std::runtime_error` as today's other
framing errors, so it is caught by the same recovery paths:

- For the three persisted index logs, `KnowledgeKernel`'s constructor catches the error (or detects
  a checkpoint mismatch — see Index checkpoint above), discards the partially-restored
  `IndexManager`, replays `assertions.log` in full, rewrites (self-heals) all three index files, and
  persists the new checkpoint.
- `AssertionLog` is not wrapped in a recovery path — a corrupted assertion log remains a fatal,
  uncaught startup error, *except* when the corruption is a tail-tolerant torn write in the active
  segment (see "Segmented assertion log" above), which is silently and safely dropped instead. A
  torn-looking anomaly in a non-active segment always throws, since it's proven impossible under
  normal operation. There is no recovery path for genuine (non-tail) `assertions.log` corruption
  because it is the system's one source of truth — nothing else to rebuild it from — so that case is
  deliberately left as a loud, fatal error requiring operator intervention (e.g. restore from
  backup).
- `EntityCatalogLog`/`PredicateCatalogLog` get the same treatment as `AssertionLog`, for the same
  reason: they are each their own source of truth for what an `EntityId`/`PredicateId` means, with
  nothing else to rebuild them from. `KnowledgeKernel`'s constructor reads both, uncaught, before the
  index/checkpoint/snapshot logic described above even runs — non-tail corruption in either file is a
  fatal startup error, not a self-heal candidate.
