#pragma once

#include <filesystem>
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

    void overwrite_all(const std::vector<SubjectIndexRecord> &record);

    std::vector<SubjectIndexRecord> read_all() const;

  private:
    std::filesystem::path path_;
};

} // namespace knk