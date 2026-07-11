#pragma once

#include <filesystem>

namespace knk {

struct StorageConfig {
    std::filesystem::path root;

    std::filesystem::path assertion_log_path() const { return root / "assertions.log"; }

    std::filesystem::path index_directory() const { return root / "indexes"; }

    std::filesystem::path observed_time_index_path() const { return index_directory() / "observed_time.idx"; }

    std::filesystem::path subject_index_path() const { return index_directory() / "subject.idx"; }

    std::filesystem::path payload_directory() const { return root / "payloads"; }
};

} // namespace knk