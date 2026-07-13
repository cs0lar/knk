#include "kernel/storage_engine.hpp"
#include "kernel/subject_index_log.hpp"

namespace knk {

StorageEngine::StorageEngine(StorageConfig config)
    : config_(std::move(config)), assertion_log_(config_.assertion_log_path()),
      observed_time_index_log_(config_.observed_time_index_path()), subject_index_log_(config_.subject_index_path()),
      current_index_log_(config_.current_index_path()), checkpoint_(config_.checkpoint_path()) {
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

void StorageEngine::append_subject_entry(EntityId subject, AssertionId id) {
    subject_index_log_.append(SubjectIndexRecord(subject, id));
}

std::vector<SubjectIndexRecord> StorageEngine::load_subject_index() const { return subject_index_log_.read_all(); }

void StorageEngine::rewrite_subject_index(const std::vector<SubjectIndexRecord> &records) {
    subject_index_log_.overwrite_all(records);
}

void StorageEngine::append_current_index_entry(EntityId subject, PredicateId predicate, AssertionId id, bool active) {
    current_index_log_.append(CurrentIndexRecord{subject, predicate, id, active});
}

std::vector<CurrentIndexRecord> StorageEngine::load_current_index() const { return current_index_log_.read_all(); }

void StorageEngine::rewrite_current_index(const std::vector<CurrentIndexRecord> &records) {
    current_index_log_.overwrite_all(records);
}

void StorageEngine::write_checkpoint(AssertionId last_fully_indexed_id) {
    checkpoint_.write(last_fully_indexed_id);
}

AssertionId StorageEngine::load_checkpoint() const { return checkpoint_.read(); }

const StorageConfig &StorageEngine::config() const { return config_; }

} // namespace knk