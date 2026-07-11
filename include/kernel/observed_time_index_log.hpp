#pragma once

#include <filesystem>
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

    void overwrite_all(const std::vector<ObservedTimeIndexRecord> &records);

    std::vector<ObservedTimeIndexRecord> read_all() const;

  private:
    std::filesystem::path path_;
};

} // namespace knk
