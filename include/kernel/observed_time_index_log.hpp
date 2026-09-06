#pragma once

#include <filesystem>
#include <span>
#include <vector>

#include "kernel/ids.hpp"
#include "kernel/time.hpp"

namespace knk {

struct ObservedTimeIndexRecord {
    EntityId subject;
    Timestamp observed_at;
    AssertionId assertion_id;
};

class ObservedTimeIndexLog {
  public:
    explicit ObservedTimeIndexLog(std::filesystem::path path);

    void append(const ObservedTimeIndexRecord &record);

    // Appends every record with one file open and one fsync, instead of one of each per
    // record -- the observed-time index's half of a batch commit's single durability boundary
    // (see KnowledgeKernel::commit_batch). Records are written in the order given; a crash
    // before the fsync returns can leave any prefix of them durable, which read_all already
    // handles as an ordinary torn trailing frame. An empty span writes nothing at all,
    // rather than creating a header-only file.
    void append_batch(std::span<const ObservedTimeIndexRecord> records);

    void overwrite_all(const std::vector<ObservedTimeIndexRecord> &records);

    std::vector<ObservedTimeIndexRecord> read_all() const;

  private:
    std::filesystem::path path_;
};

} // namespace knk
