#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <vector>

#include "kernel/assertion.hpp"
#include "kernel/ids.hpp"

namespace knk {

struct SnapshotData {
    AssertionId last_snapshotted_id;
    std::vector<Assertion> assertions;
};

// Persists a single full-replace snapshot of assertions_ (unlike the four append-only logs, always
// fully rewritten via write_file_atomically, never appended to). Purely an optimization hint for
// KnowledgeKernel's startup fast path, never authoritative: read() never throws, degrading any
// anomaly (missing file, bad header, bad checksum, inconsistent record count, or a format version this
// build does not know) to "no usable snapshot", same philosophy as IndexCheckpoint.
//
// **Format v2 stores each record's *appended* status**, matching assertions.log, where v1 stored the
// effective one. The difference matters because effective status is lossy: a superseded row reads
// `Superseded` whether it was committed Active or as a Hypothesis, so a snapshot of it could not
// reconstruct what was believed before the supersession, and the fast path would answer an as-of query
// differently from a full replay (see status_history.hpp). The kernel re-derives effective status after
// loading, from the supersedes/retracts links the records already carry. A v1 snapshot is simply not
// usable to a v2 build, which costs one slow startup and no data -- the log is the source of truth.
class SnapshotStore {
  public:
    explicit SnapshotStore(std::filesystem::path path);

    // `appended_status` is parallel to `assertions` and supplies the status byte actually written, since
    // the in-memory records carry the effective one. Must be the same length as `assertions`.
    void write(AssertionId last_snapshotted_id, const std::vector<Assertion> &assertions,
               std::span<const uint8_t> appended_status);

    std::optional<SnapshotData> read() const;

  private:
    std::filesystem::path path_;
};

} // namespace knk
