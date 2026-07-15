#pragma once

#include <cstdio>
#include <filesystem>
#include <string>

namespace knk {

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

    std::filesystem::path observed_time_index_path() const { return index_directory() / "observed_time.idx"; }

    std::filesystem::path subject_index_path() const { return index_directory() / "subject.idx"; }

    std::filesystem::path current_index_path() const { return index_directory() / "current.idx"; }

    std::filesystem::path checkpoint_path() const { return index_directory() / "checkpoint"; }

    std::filesystem::path snapshot_path() const { return root / "snapshot"; }

    std::filesystem::path payload_directory() const { return root / "payloads"; }

    std::filesystem::path catalog_directory() const { return root / "catalog"; }

    std::filesystem::path entity_catalog_path() const { return catalog_directory() / "entities.log"; }

    std::filesystem::path predicate_catalog_path() const { return catalog_directory() / "predicates.log"; }
};

} // namespace knk