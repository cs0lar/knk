#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>

#include "kernel/ids.hpp"

namespace knk {

// How a storage root is opened (Phase 13).
//
// ReadWrite is everything the kernel did before this existed: it takes the exclusive writer lock on
// the root, creates the directory layout if absent, and may repair derived state (rebuilding and
// rewriting a stale or corrupt index, moving the checkpoint forward).
//
// ReadOnly takes **no** lock at all, and that is deliberate rather than lazy. The obvious design --
// readers take flock(LOCK_SH) on the same LOCK file -- cannot work: the writer holds LOCK_EX on that
// file for its whole lifetime, and LOCK_EX excludes LOCK_SH, so a "shared lock" reader could never
// open alongside the writer it is meant to coexist with. Since a reader cannot corrupt anything it
// never writes to, the honest design is that LOCK stays purely the single-writer guard and readers
// do not participate in it. If some future destructive operation (a compaction, say) needs to wait
// for readers to drain, that wants its own reader-presence lock, added then.
//
// A ReadOnly open creates nothing -- not the root, not a subdirectory, not a lock file -- and every
// write path throws instead of writing. See docs/storage_format.md's "Storage root lock" section.
enum class OpenMode : uint8_t { ReadWrite, ReadOnly };

struct StorageConfig {
    std::filesystem::path root;

    // Soft capacity bound per assertion-log segment file, not a tuned performance number -- nothing
    // above AssertionLog observes segment boundaries, so this is a storage-layout default.
    size_t max_records_per_segment = 100'000;

    std::filesystem::path segment_directory() const { return root / "segments"; }

    std::filesystem::path segment_path(size_t segment_index) const {
        char buffer[11];
        std::snprintf(buffer, sizeof(buffer), "%010zu", segment_index);
        return segment_directory() / (std::string(buffer) + ".seg");
    }

    std::filesystem::path index_directory() const { return root / "indexes"; }

    // The columnar projection (Phase 14). Derived state, like indexes/, and rebuildable from the
    // assertion log -- see column_store.hpp.
    std::filesystem::path column_directory() const { return root / "columns"; }

    std::filesystem::path observed_time_index_path() const { return index_directory() / "observed_time.idx"; }

    std::filesystem::path subject_index_path() const { return index_directory() / "subject.idx"; }

    std::filesystem::path current_index_path() const { return index_directory() / "current.idx"; }

    std::filesystem::path checkpoint_path() const { return index_directory() / "checkpoint"; }

    std::filesystem::path snapshot_path() const { return root / "snapshot"; }

    std::filesystem::path payload_directory() const { return root / "payloads"; }

    std::filesystem::path payload_path(EntityId id) const {
        return payload_directory() / (std::to_string(id) + ".payload");
    }

    std::filesystem::path catalog_directory() const { return root / "catalog"; }

    std::filesystem::path entity_catalog_path() const { return catalog_directory() / "entities.log"; }

    std::filesystem::path predicate_catalog_path() const { return catalog_directory() / "predicates.log"; }

    std::filesystem::path entity_merge_log_path() const { return catalog_directory() / "entity_merges.log"; }

    std::filesystem::path provenance_directory() const { return root / "provenance"; }

    std::filesystem::path provenance_log_path() const { return provenance_directory() / "provenance.log"; }
};

} // namespace knk