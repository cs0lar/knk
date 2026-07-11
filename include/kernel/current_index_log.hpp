#pragma once

#include <filesystem>
#include <vector>

#include "kernel/ids.hpp"

namespace knk {

struct CurrentIndexRecord {
    EntityId subject;
    PredicateId predicate;
    AssertionId assertion_id;
    bool active;
};

class CurrentIndexLog {
  public:
    explicit CurrentIndexLog(std::filesystem::path path);

    void append(const CurrentIndexRecord &record);

    void overwrite_all(const std::vector<CurrentIndexRecord> &records);

    std::vector<CurrentIndexRecord> read_all() const;

  private:
    std::filesystem::path path_;
};

} // namespace knk
