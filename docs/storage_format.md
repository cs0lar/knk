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
a missing/empty file, and at the start of every `overwrite_all()`). A file that is non-empty but
has a missing or mismatched magic, or an unrecognized version, is treated as corrupt and
`read_all()` throws `std::runtime_error`.

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
  write, not corruption. If a frame is fully present but the recomputed CRC does not match the
  stored one, `read_all()` throws `std::runtime_error` — this is real corruption, not a torn write.

### Record payloads

- `AssertionLog`: raw `knk::Assertion` (`include/kernel/assertion.hpp`) — `id`, `subject`,
  `predicate`, `object`, `valid_from`, `valid_to`, `observed_at`, `confidence`, `status`,
  `supersedes_id`, `retracts_id`.
- `SubjectIndexLog`: `SubjectIndexRecord` — `{subject, assertion_id}`.
- `CurrentIndexLog`: `CurrentIndexRecord` — `{subject, predicate, assertion_id, active}`.
- `ObservedTimeIndexLog`: `ObservedTimeIndexRecord` — `{subject, observed_at, assertion_id}`.

## Known limitation: raw struct serialization

Record payloads are still written via `reinterpret_cast`-style raw struct serialization, not
explicit field-by-field encoding. This means the on-disk format is sensitive to compiler ABI,
struct padding, enum underlying size, and host endianness — a log written on one platform/compiler
is not guaranteed to be portable to another. This is an accepted limitation for early local
development (per `AGENTS.md`'s Binary Format Rules) and is unchanged by the introduction of
checksums; replacing it with explicit serialization is a larger, separate Phase 4 concern.

## Recovery behavior

A checksum failure throws the same `std::runtime_error` as today's other framing errors, so it is
caught by the same recovery paths:

- For the three persisted index logs, `KnowledgeKernel`'s constructor catches the error, discards
  the partially-restored `IndexManager`, replays `assertions.log` in full, and rewrites
  (self-heals) all three index files.
- `AssertionLog` is not wrapped in a recovery path — a corrupted assertion log remains a fatal,
  uncaught startup error. Giving the log of record itself a recovery path is a "Crash recovery"
  concern (a separate, later Phase 4 item), not something checksums attempt to solve.
