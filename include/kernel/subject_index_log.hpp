#pragma once

#include <filesystem>
#include <span>
#include <vector>

#include "kernel/ids.hpp"

namespace knk {

struct SubjectIndexRecord {
    EntityId subject;
    AssertionId assertion_id;
};

class SubjectIndexLog {
  public:
    explicit SubjectIndexLog(std::filesystem::path path);

    void append(const SubjectIndexRecord &record);

    // Appends every record with one file open and one fsync, instead of one of each per
    // record -- the subject index's half of a batch commit's single durability boundary
    // (see KnowledgeKernel::commit_batch). Records are written in the order given; a crash
    // before the fsync returns can leave any prefix of them durable, which read_all already
    // handles as an ordinary torn trailing frame. An empty span writes nothing at all,
    // rather than creating a header-only file.
    void append_batch(std::span<const SubjectIndexRecord> records);

    void overwrite_all(const std::vector<SubjectIndexRecord> &record);

    std::vector<SubjectIndexRecord> read_all() const;

  private:
    std::filesystem::path path_;
};

} // namespace knk