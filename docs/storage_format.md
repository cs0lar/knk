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
