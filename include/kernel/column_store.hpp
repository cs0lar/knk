#pragma once

// A column-oriented projection of the assertion log (Phase 14): eight fixed-width arrays, one per
// Assertion field, in assertion-id order -- row i is id i+1. Memory-mapped for scanning, which is the
// substrate the vectorized execution in Phase 15 needs and which a row-of-structs layout cannot give:
// a filter on one field touches one byte range instead of striding over 88-byte records.
//
// **Derived state, and nothing more.** Every byte here is reconstructible from `assertions.log`, so
// corruption gets the Phase 3 index treatment rather than AssertionLog's: discard and rebuild, never
// fatal, never authoritative. Nothing in the kernel may treat a column as a source of truth, and a
// query answered from columns must be answerable identically from the log.
//
// The projection is **verbatim**: each row is the log record as appended, including the status it was
// appended with. It is not a projection of replayed in-memory state, which is a different thing -- a
// superseded row's record still says Active, because append-only storage never rewrote it. Effective
// status is derived from the supersedes_id/retracts_id columns, exactly as replay derives it. Keeping the
// projection verbatim is what lets an append stay an append: mirroring effective status instead would
// mean rewriting an arbitrary earlier row on every supersession, which for a fixed-stride column means
// an in-place write and a full checksum recomputation.
//
// The integrity story differs from every other file in the storage root, deliberately. The logs carry a
// CRC per record, which fixed-stride columns cannot do without giving up the stride that makes them
// worth having. Instead `columns/manifest` records the row count and one CRC per column, and the whole
// store is verified against it on open; a mismatch means rebuild. The cost is an O(rows) read at open,
// which is measured in docs/benchmarks.md rather than assumed to be free.
//
// Not covered here: reading columns during query execution. Phase 14 maintains and proves the store;
// Phase 15 is what makes queries use it.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <vector>

#include "kernel/assertion.hpp"
#include "kernel/ids.hpp"
#include "kernel/time.hpp"

namespace knk {

// Borrowed views over the mapped columns, all of the same length (ColumnStore::row_count). Valid until
// the next append/overwrite_all/unmap on the store that produced them -- those remap the files, which
// invalidates every pointer handed out earlier.
struct ColumnSpans {
    std::span<const EntityId> subject;
    std::span<const PredicateId> predicate;
    std::span<const EntityId> object;
    std::span<const Timestamp> valid_from;
    std::span<const Timestamp> valid_to;
    std::span<const Timestamp> observed_at;
    std::span<const double> confidence;

    // AssertionStatus as its underlying value. The enum's numeric values are therefore part of the
    // on-disk format -- see the explicit initializers in status.hpp, which exist to say so.
    //
    // **This is the status the record was appended with, not the row's effective status today.** The log
    // is append-only: a supersession does not rewrite the row it supersedes, it appends a new row whose
    // supersedes_id points back at it, and the older row's Superseded status exists only in replayed
    // in-memory state. A column scan that filtered on this byte alone would report superseded and
    // retracted rows as Active.
    std::span<const uint8_t> status;

    // Why the two link columns are here: together with status they make effective status derivable from
    // the columns alone, with no side table -- a row is superseded or retracted exactly when some later
    // row points at it. That is the same derivation replay performs, and the mechanism Phase 19's
    // as-of-commit reconstruction needs (restrict the scan to ids <= N and the answer is the status as
    // of commit N).
    std::span<const AssertionId> supersedes_id;
    std::span<const AssertionId> retracts_id;

    bool empty() const { return subject.empty(); }
};

class ColumnStore {
  public:
    explicit ColumnStore(std::filesystem::path directory);

    ~ColumnStore();

    ColumnStore(const ColumnStore &) = delete;
    ColumnStore &operator=(const ColumnStore &) = delete;

    // Movable, because StorageEngine holds one and KnowledgeKernel is returned by value in places (see
    // benchmarks/query_benchmark.cpp) -- a non-movable member here would delete the kernel's move
    // constructor transitively. A move transfers the open descriptors and mappings and leaves the source
    // holding nothing, so exactly one object ever unmaps them.
    ColumnStore(ColumnStore &&other) noexcept;
    ColumnStore &operator=(ColumnStore &&other) noexcept;

    // Rows the manifest claims, which is the number of rows callers may rely on. Zero for a store that
    // does not exist yet, or one whose manifest is missing or unreadable.
    size_t row_count() const;

    // Appends rows in the order given, then rewrites the manifest and fsyncs. Assertions must arrive in
    // id order, contiguous with what is already stored -- the caller is StorageEngine, appending exactly
    // what it just appended to the assertion log.
    void append(std::span<const Assertion> assertions);

    // Rebuilds every column from scratch. The recovery path: cheaper to reason about than repairing a
    // store whose columns disagree with each other.
    void overwrite_all(std::span<const Assertion> assertions);

    // True when every column's size and CRC match the manifest. Reads every byte, so it is the open-time
    // cost this design trades for not having per-row checksums.
    bool verify() const;

    // Maps every column for reading. Returns empty spans when the store is unusable (absent, or failing
    // verify); a caller that gets empty spans falls back to the log, which is always authoritative.
    ColumnSpans map();

    void unmap();

  private:
    struct Mapping {
        int fd = -1;
        void *address = nullptr;
        size_t length = 0;
    };

    std::filesystem::path column_path(size_t column) const;
    void load_manifest();
    void write_manifest(const std::vector<uint32_t> &checksums);
    std::vector<uint32_t> compute_checksums() const;

    std::filesystem::path directory_;
    size_t row_count_ = 0;

    // As loaded from (or last written to) the manifest, in column order. Empty means "no usable
    // manifest", which every caller answers by rebuilding. Kept so an append can continue each column's
    // checksum instead of recomputing it over every row.
    std::vector<uint32_t> manifest_checksums_;

    std::vector<Mapping> mappings_;
};

} // namespace knk
