#pragma once

#include <filesystem>
#include <vector>

#include "kernel/assertion.hpp"
#include "kernel/ids.hpp"

namespace knk {

// Stores assertion records across a directory of fixed-capacity segment files (segments/NNNNNNNNNN.seg)
// instead of one ever-growing file. Segment index k deterministically holds ids
// [k*max_records_per_segment + 1, (k+1)*max_records_per_segment]: capacity is checked before writing a
// record, so a segment is only ever rolled from after its last record was already fully appended and
// fsynced in an earlier call, meaning every non-active segment is guaranteed exactly
// max_records_per_segment complete records. Only the single active (highest-index) segment can be short
// or have a torn trailing frame from a crash. This makes segment boundaries pure arithmetic -- no
// manifest/metadata file needed.
class AssertionLog {
  public:
    explicit AssertionLog(std::filesystem::path segment_directory, size_t max_records_per_segment);

    void append(const Assertion &assertion);

    std::vector<Assertion> read_all() const;

    // Returns only assertions with id > last_seen_id, skipping whole segments known (by index
    // arithmetic) to be entirely before it and seeking within the one segment straddling it, instead
    // of parsing every segment from the start. read_after(0) behaves like read_all().
    std::vector<Assertion> read_after(AssertionId last_seen_id) const;

    // Cheap estimate of how many records the log holds: exact for every non-active segment (always
    // exactly full, see class comment), a file-size-based estimate for the active one. Used only as a
    // sanity check against stale/bogus hints (e.g. a snapshot claiming to cover more records than the
    // log could possibly contain); never authoritative.
    AssertionId record_count_hint() const;

  private:
    std::filesystem::path segment_directory_;
    size_t max_records_per_segment_;
    size_t active_segment_index_;
    size_t active_segment_count_;
};

} // namespace knk