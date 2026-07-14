#pragma once

#include <filesystem>
#include <vector>

#include "kernel/assertion.hpp"
#include "kernel/ids.hpp"

namespace knk {

class AssertionLog {
  public:
    explicit AssertionLog(std::filesystem::path path);

    void append(const Assertion &assertion);

    std::vector<Assertion> read_all() const;

    // Returns only assertions with id > last_seen_id, seeking directly to the deterministic byte
    // offset for that id instead of parsing the whole file. read_after(0) behaves like read_all().
    std::vector<Assertion> read_after(AssertionId last_seen_id) const;

    // Cheap file-size-based estimate of how many records the log holds, with no parsing. Used only
    // as a sanity check against stale/bogus hints (e.g. a snapshot claiming to cover more records
    // than the log could possibly contain); never authoritative.
    AssertionId record_count_hint() const;

  private:
    std::filesystem::path path_;
};

} // namespace knk