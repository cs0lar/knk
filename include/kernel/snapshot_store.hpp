#pragma once

#include <filesystem>
#include <optional>
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
// anomaly (missing file, bad header, bad checksum, inconsistent record count) to "no usable
// snapshot", same philosophy as IndexCheckpoint.
class SnapshotStore {
  public:
    explicit SnapshotStore(std::filesystem::path path);

    void write(AssertionId last_snapshotted_id, const std::vector<Assertion> &assertions);

    std::optional<SnapshotData> read() const;

  private:
    std::filesystem::path path_;
};

} // namespace knk
