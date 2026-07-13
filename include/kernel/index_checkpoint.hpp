#pragma once

#include <filesystem>

#include "kernel/ids.hpp"

namespace knk {

// Tracks the highest AssertionId whose index-log writes are known to have fully completed.
// Purely an optimization hint for KnowledgeKernel's startup fast path, never authoritative data:
// unlike the append-only logs, read() never throws. A missing or corrupt file just means "nothing
// confirmed" (0), which costs an extra full replay on next startup but never loses data.
class IndexCheckpoint {
  public:
    explicit IndexCheckpoint(std::filesystem::path path);

    void write(AssertionId last_fully_indexed_id);

    AssertionId read() const;

  private:
    std::filesystem::path path_;
};

} // namespace knk
