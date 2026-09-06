#pragma once

#include <filesystem>
#include <span>
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

    // Appends every record with one file open and one fsync per segment touched, instead of one of
    // each per record -- the durability boundary a batch commit is built on (see
    // KnowledgeKernel::commit_batch). Records are written in the order given, and a segment is filled
    // to capacity and fsynced before the next one is opened, so the class's "every non-active segment
    // holds exactly max_records_per_segment complete records" invariant holds mid-batch exactly as it
    // does between single appends.
    //
    // This is not an atomic multi-record write and does not pretend to be: a crash before the fsync
    // returns can leave any prefix of the batch durable (0 to all of it). It can only ever be a
    // prefix, never a gap or a reordering, because records are written in order and only the trailing
    // frame of the active segment can be torn -- read_all/read_after drop exactly that one frame.
    void append_batch(std::span<const Assertion> assertions);

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

    // Moves every already-rolled-from segment entirely before assertion_id (i.e. every id it holds is
    // < assertion_id) from segments/ into segments/archive/, skipping the active segment unconditionally
    // (it may still receive writes, so it is never "fully rolled" in the sense this invariant relies
    // on). This is compaction, not deletion: read_all/read_after transparently see archived segments
    // exactly as before, and repeated calls are a no-op for anything already moved.
    void archive_segments_before(AssertionId assertion_id);

  private:
    std::filesystem::path segment_directory_;
    size_t max_records_per_segment_;
    size_t active_segment_index_;
    size_t active_segment_count_;
};

} // namespace knk