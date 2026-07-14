# Storage Format

This document describes the on-disk binary format shared by all four persisted logs:
`assertions.log` (`AssertionLog`), `indexes/subject.idx` (`SubjectIndexLog`),
`indexes/current.idx` (`CurrentIndexLog`), and `indexes/observed_time.idx` (`ObservedTimeIndexLog`).

All four logs use the identical framing described below; only the record payload struct differs
per log.

## File layout

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

- `record_size` is redundant with the record's compile-time `sizeof(...)` (all four record types
  are fixed-size) and exists purely as an early sanity check; a mismatch throws
  `std::runtime_error`.
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

### Record payloads

- `AssertionLog`: raw `knk::Assertion` (`include/kernel/assertion.hpp`) — `id`, `subject`,
  `predicate`, `object`, `valid_from`, `valid_to`, `observed_at`, `confidence`, `status`,
  `supersedes_id`, `retracts_id`.
- `SubjectIndexLog`: `SubjectIndexRecord` — `{subject, assertion_id}`.
- `CurrentIndexLog`: `CurrentIndexRecord` — `{subject, predicate, assertion_id, active}`.
- `ObservedTimeIndexLog`: `ObservedTimeIndexRecord` — `{subject, observed_at, assertion_id}`.

## Durability

Every `append()` closes its `std::ofstream` and then fsyncs the file (`knk::fsync_file`,
`include/kernel/durability.hpp`) before returning, so a commit is not considered durable until the
data has actually reached physical disk, not just the OS page cache.

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
  uncaught startup error, *except* when the corruption is a tail-tolerant torn write (see the
  checksum-mismatch tail-tolerance policy above), which is silently and safely dropped instead.
  There is no recovery path for genuine (non-tail) `assertions.log` corruption because it is the
  system's one source of truth — nothing else to rebuild it from — so that case is deliberately
  left as a loud, fatal error requiring operator intervention (e.g. restore from backup).
