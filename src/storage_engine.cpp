#include <kernel/storage_engine.hpp>

namespace knk {

StorageEngine::StorageEngine(StorageConfig config)
    : config_(std::move(config)), assertion_log_(config_.assertion_log_path()) {
    std::filesystem::create_directories(config_.root);
    std::filesystem::create_directories(config_.index_directory());
    std::filesystem::create_directories(config_.payload_directory());
}

void StorageEngine::append_assertion(const Assertion &assertion) { assertion_log_.append(assertion); }

std::vector<Assertion> StorageEngine::load_assertions() const { return assertion_log_.read_all(); }

const StorageConfig &StorageEngine::config() const { return config_; }

} // namespace knk