#pragma once

#include <filesystem>
#include <vector>

#include "kernel/assertion.hpp"
#include "kernel/assertion_log.hpp"
#include "kernel/storage_config.hpp"

namespace knk {

class StorageEngine {
  public:
    explicit StorageEngine(StorageConfig config);

    void append_assertion(const Assertion &assertion);

    std::vector<Assertion> load_assertions() const;

    const StorageConfig &config() const;

  private:
    StorageConfig config_;
    AssertionLog assertion_log_;
};

} // namespace knk