#include "kernel/storage_engine.hpp"

namespace knk {

StorageEngine::StorageEngine(StorageConfig config)
    : config_(std::move(config)), assertion_log_(config_.assertion_log_path()),
      observed_time_index_log_(config_.observed_time_index_path()) {
    std::filesystem::create_directories(config_.root);
    std::filesystem::create_directories(config_.index_directory());
    std::filesystem::create_directories(config_.payload_directory());
}

void StorageEngine::append_assertion(const Assertion &assertion) { assertion_log_.append(assertion); }

std::vector<Assertion> StorageEngine::load_assertions() const { return assertion_log_.read_all(); }

void StorageEngine::append_observed_time_entry(EntityId subject, Timestamp observed_at, AssertionId id) {
    observed_time_index_log_.append(ObservedTimeIndexRecord{subject, observed_at, id});
}

std::vector<ObservedTimeIndexRecord> StorageEngine::load_observed_time_index() const {
    return observed_time_index_log_.read_all();
}

void StorageEngine::rewrite_observed_time_index(const std::vector<ObservedTimeIndexRecord> &records) {
    observed_time_index_log_.overwrite_all(records);
}

const StorageConfig &StorageEngine::config() const { return config_; }

} // namespace knk